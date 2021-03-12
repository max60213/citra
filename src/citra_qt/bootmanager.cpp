// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QApplication>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWindow>
#include <QScreen>
#include <QWindow>
#include <fmt/format.h>
#include "citra_qt/bootmanager.h"
#include "citra_qt/main.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/frontend/scope_acquire_context.h"
#include "core/settings.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "network/network.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

EmuThread::EmuThread(Frontend::GraphicsContext& core_context) : core_context(core_context) {}

EmuThread::~EmuThread() = default;

static GMainWindow* GetMainWindow() {
    for (QWidget* w : qApp->topLevelWidgets()) {
        if (GMainWindow* main = qobject_cast<GMainWindow*>(w)) {
            return main;
        }
    }
    return nullptr;
}

void EmuThread::run() {
    MicroProfileOnThreadCreate("EmuThread");
    Frontend::ScopeAcquireContext scope(core_context);

    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);

    Core::System::GetInstance().Renderer().Rasterizer()->LoadDiskResources(
        stop_run, [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            emit LoadProgress(stage, value, total);
        });

    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);

    // Holds whether the cpu was running during the last iteration,
    // so that the DebugModeLeft signal can be emitted before the
    // next execution step.
    bool was_active = false;
    while (!stop_run) {
        if (running) {
            if (!was_active)
                emit DebugModeLeft();

            Core::System::ResultStatus result = Core::System::GetInstance().RunLoop();
            if (result == Core::System::ResultStatus::ShutdownRequested) {
                // Notify frontend we shutdown
                emit ErrorThrown(result, "");
                // End emulation execution
                break;
            }
            if (result != Core::System::ResultStatus::Success) {
                this->SetRunning(false);
                emit ErrorThrown(result, Core::System::GetInstance().GetStatusDetails());
            }

            was_active = running || exec_step;
            if (!was_active && !stop_run)
                emit DebugModeEntered();
        } else if (exec_step) {
            if (!was_active)
                emit DebugModeLeft();

            exec_step = false;
            Core::System::GetInstance().SingleStep();
            emit DebugModeEntered();
            yieldCurrentThread();

            was_active = false;
        } else {
            std::unique_lock lock{running_mutex};
            running_cv.wait(lock, [this] { return IsRunning() || exec_step || stop_run; });
        }
    }

    // Shutdown the core emulation
    Core::System::GetInstance().Shutdown();

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadExit();
#endif
}

OpenGLWindow::OpenGLWindow(QWindow* parent, QWidget* event_handler, QOpenGLContext* shared_context)
    : QWindow(parent), context(new QOpenGLContext(shared_context->parent())),
      event_handler(event_handler) {

    // disable vsync for any shared contexts
    auto format = shared_context->format();
    format.setSwapInterval(Settings::values.use_vsync_new ? 1 : 0);
    this->setFormat(format);

    context->setShareContext(shared_context);
    context->setScreen(this->screen());
    context->setFormat(format);
    context->create();

    setSurfaceType(QWindow::OpenGLSurface);

    // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground,
    // WA_DontShowOnScreen, WA_DeleteOnClose
}

OpenGLWindow::~OpenGLWindow() {
    context->doneCurrent();
}

void OpenGLWindow::Present() {
    if (!isExposed())
        return;

    context->makeCurrent(this);
    if (VideoCore::g_renderer) {
        VideoCore::g_renderer->TryPresent(100);
    }
    context->swapBuffers(this);
    auto f = context->versionFunctions<QOpenGLFunctions_3_3_Core>();
    f->glFinish();
    QWindow::requestUpdate();
}

bool OpenGLWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::UpdateRequest:
        Present();
        return true;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::FocusAboutToChange:
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::Wheel:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::InputMethodQuery:
    case QEvent::TouchCancel:
        return QCoreApplication::sendEvent(event_handler, event);
    case QEvent::Drop:
        GetMainWindow()->DropAction(static_cast<QDropEvent*>(event));
        return true;
    case QEvent::DragEnter:
    case QEvent::DragMove:
        GetMainWindow()->AcceptDropEvent(static_cast<QDropEvent*>(event));
        return true;
    default:
        return QWindow::event(event);
    }
}

void OpenGLWindow::exposeEvent(QExposeEvent* event) {
    QWindow::requestUpdate();
    QWindow::exposeEvent(event);
}

GRenderWindow::GRenderWindow(QWidget* parent_, EmuThread* emu_thread)
    : QWidget(parent_), emu_thread(emu_thread) {

    setWindowTitle(QStringLiteral("Citra %1 | %2-%3")
                       .arg(QString::fromUtf8(Common::g_build_name),
                            QString::fromUtf8(Common::g_scm_branch),
                            QString::fromUtf8(Common::g_scm_desc)));
    setAttribute(Qt::WA_AcceptTouchEvents);
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);
    InputCommon::Init();

    this->setMouseTracking(true);

    GMainWindow* parent = GetMainWindow();
    connect(this, &GRenderWindow::FirstFrameDisplayed, parent, &GMainWindow::OnLoadComplete);
}

GRenderWindow::~GRenderWindow() {
    InputCommon::Shutdown();
}

void GRenderWindow::MakeCurrent() {
    core_context->MakeCurrent();
}

void GRenderWindow::DoneCurrent() {
    core_context->DoneCurrent();
}

void GRenderWindow::PollEvents() {
    if (!first_frame) {
        first_frame = true;
        emit FirstFrameDisplayed();
    }
}

// On Qt 5.0+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::OnFramebufferSizeChanged() {
    // Screen changes potentially incur a change in screen DPI, hence we should update the
    // framebuffer size
    const qreal pixel_ratio = windowPixelRatio();
    const u32 width = this->width() * pixel_ratio;
    const u32 height = this->height() * pixel_ratio;
    UpdateCurrentFramebufferLayout(width, height);
}

void GRenderWindow::BackupGeometry() {
    geometry = QWidget::saveGeometry();
}

void GRenderWindow::RestoreGeometry() {
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry) {
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry() {
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == nullptr) {
        return QWidget::saveGeometry();
    }

    return geometry;
}

qreal GRenderWindow::windowPixelRatio() const {
    return devicePixelRatioF();
}

std::pair<u32, u32> GRenderWindow::ScaleTouch(const QPointF pos) const {
    const qreal pixel_ratio = windowPixelRatio();
    return {static_cast<u32>(std::max(std::round(pos.x() * pixel_ratio), qreal{0.0})),
            static_cast<u32>(std::max(std::round(pos.y() * pixel_ratio), qreal{0.0}))};
}

void GRenderWindow::closeEvent(QCloseEvent* event) {
    emit Closed();
    QWidget::closeEvent(event);
}

void GRenderWindow::keyPressEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->PressKey(event->key());
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->ReleaseKey(event->key());
}

void GRenderWindow::mousePressEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchBeginEvent

    auto pos = event->pos();
    if (event->button() == Qt::LeftButton) {
        const auto [x, y] = ScaleTouch(pos);
        this->TouchPressed(x, y);
    } else if (event->button() == Qt::RightButton) {
        InputCommon::GetMotionEmu()->BeginTilt(pos.x(), pos.y());
    }
    emit MouseActivity();
}

void GRenderWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchUpdateEvent

    auto pos = event->pos();
    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
    InputCommon::GetMotionEmu()->Tilt(pos.x(), pos.y());
    emit MouseActivity();
}

void GRenderWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchEndEvent

    if (event->button() == Qt::LeftButton)
        this->TouchReleased();
    else if (event->button() == Qt::RightButton)
        InputCommon::GetMotionEmu()->EndTilt();
    emit MouseActivity();
}

void GRenderWindow::TouchBeginEvent(const QTouchEvent* event) {
    // TouchBegin always has exactly one touch point, so take the .first()
    const auto [x, y] = ScaleTouch(event->touchPoints().first().pos());
    this->TouchPressed(x, y);
}

void GRenderWindow::TouchUpdateEvent(const QTouchEvent* event) {
    QPointF pos;
    int active_points = 0;

    // average all active touch points
    for (const auto& tp : event->touchPoints()) {
        if (tp.state() & (Qt::TouchPointPressed | Qt::TouchPointMoved | Qt::TouchPointStationary)) {
            active_points++;
            pos += tp.pos();
        }
    }

    pos /= active_points;

    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
}

void GRenderWindow::TouchEndEvent() {
    this->TouchReleased();
}

bool GRenderWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::TouchBegin:
        TouchBeginEvent(static_cast<QTouchEvent*>(event));
        return true;
    case QEvent::TouchUpdate:
        TouchUpdateEvent(static_cast<QTouchEvent*>(event));
        return true;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        TouchEndEvent();
        return true;
    default:
        break;
    }

    return QWidget::event(event);
}

void GRenderWindow::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    InputCommon::GetKeyboard()->ReleaseAllKeys();
}

void GRenderWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    OnFramebufferSizeChanged();
}

void GRenderWindow::InitRenderTarget() {
    ReleaseRenderTarget();

    first_frame = false;

    GMainWindow* parent = GetMainWindow();
    QWindow* parent_win_handle = parent ? parent->windowHandle() : nullptr;
    child_window = new OpenGLWindow(parent_win_handle, this, QOpenGLContext::globalShareContext());
    child_window->create();
    child_widget = createWindowContainer(child_window, this);
    child_widget->resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);

    layout()->addWidget(child_widget);

    core_context = CreateSharedContext();
    resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);
    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    OnFramebufferSizeChanged();
    BackupGeometry();
}

void GRenderWindow::ReleaseRenderTarget() {
    if (child_widget) {
        layout()->removeWidget(child_widget);
        delete child_widget;
        child_widget = nullptr;
    }
}

void GRenderWindow::CaptureScreenshot(u32 res_scale, const QString& screenshot_path) {
    if (res_scale == 0)
        res_scale = VideoCore::GetResolutionScaleFactor();
    const Layout::FramebufferLayout layout{Layout::FrameLayoutFromResolutionScale(res_scale)};
    screenshot_image = QImage(QSize(layout.width, layout.height), QImage::Format_RGB32);
    VideoCore::RequestScreenshot(
        screenshot_image.bits(),
        [=] {
            const std::string std_screenshot_path = screenshot_path.toStdString();
            if (screenshot_image.mirrored(false, true).save(screenshot_path)) {
                LOG_INFO(Frontend, "Screenshot saved to \"{}\"", std_screenshot_path);
            } else {
                LOG_ERROR(Frontend, "Failed to save screenshot to \"{}\"", std_screenshot_path);
            }
        },
        layout);
}


#define PORT 6543

#include <QTcpSocket>
#include <QAbstractSocket>
int socketSend(QTcpSocket& sock, const char *data, int sz) {
    if (sock.state() != QAbstractSocket::ConnectedState) return 0;

    sock.write(data, sz);
    sock.waitForBytesWritten();

    return 1;
}

uint8_t readConfirmation(QTcpSocket& sock) {
    sock.waitForReadyRead(0);
    if (sock.bytesAvailable() > 0) {
        char result;
        int rd = sock.read(&result, 1);
        if (rd > 0) return result;
    }

    return 0;
}

#include <QBuffer>
#define DECLJPEGOUTBUF(var) QByteArray var
const char *jpegCompress(unsigned char *data, int width, int height, int quality, QByteArray *outBuf, unsigned long *outSize) {
    QImage img = QImage(data, width, height, QImage::Format_RGB888);

    outBuf->resize(0);
    QBuffer buffer(outBuf);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "JPG", quality);
    buffer.close();

    *outSize = outBuf->size();
    return outBuf->constData();
}

//#define MIN_SQDIFF 128
//#define MIN_SQDIFF (8 * 8 * 3 * 2)
#define MIN_SQDIFF (8 * 8 * 3)
int squareDiff(unsigned char *ptr1, unsigned char *ptr2, int rowStride) {
    int diff = 0;

    for (int i=0; i<8; i++) {
        for (int j=0; j<8; j++) {
            diff += abs(ptr1[0] - ptr2[0]);
            diff += abs(ptr1[1] - ptr2[1]);
            diff += abs(ptr1[2] - ptr2[2]);
            if (diff > MIN_SQDIFF) return 1;
            ptr1 += 3;
            ptr2 += 3;
        }
        ptr1 += rowStride - (8 * 3);
        ptr2 += rowStride - (8 * 3);
    }

    return 0;
}

int copySquare(unsigned char *dst, unsigned char *src, int rowStride) {
    for (int i=0; i<8; i++) {
        for (int j=0; j<8; j++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst += 3;
            src += 3;
        }
        dst += rowStride - (8 * 3);
        src += rowStride - (8 * 3);
    }

    return 0;
}


unsigned char diffBuf[240*320*3];
unsigned char diffMap[((240 / 8) * (320 / 8)) / 8];
int putSquare(unsigned char *dst, unsigned char *src, int rowStride) {
    for (int i=0; i<8; i++) {
        for (int j=0; j<8; j++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst += 3;
            src += 3;
        }
        src += rowStride - (8 * 3);
    }

    return 0;
}

unsigned char *lastImage = 0;
int16_t imageDiff(unsigned char *currentImage, int width, int height) {
    int16_t numSqDiff = 0;
    int rowStride = width * 3;
    int mapPos = 0;
    int mapMask = 0x01;

    if (lastImage == 0) {
        lastImage = (unsigned char *) malloc(rowStride * height);
        memcpy(lastImage, currentImage, rowStride * height);
        return -1;
    } else {
        unsigned char *lPtr = lastImage;
        unsigned char *cPtr = currentImage;

        for (int i=0; i<height; i+=8) {
            for (int j=0; j<width; j+=8) {
                int sDiff = squareDiff(lPtr, cPtr, rowStride);
                if (sDiff) {
                    diffMap[mapPos] |= mapMask;
                    copySquare(lPtr, cPtr, rowStride);
                    putSquare(diffBuf + 8 * 8 * 3 * numSqDiff, cPtr, rowStride);
                    ++numSqDiff;
                } else {
                    diffMap[mapPos] &= ~mapMask;
                }
                if (mapMask == 0x80) {mapMask = 0x01; mapPos++;}
                else mapMask <<= 1;
                lPtr += 8 * 3;
                cPtr += 8 * 3;
            }
            lPtr += (8 * rowStride) - rowStride;
            cPtr += (8 * rowStride) - rowStride;
        }
    }

    return numSqDiff;
}

uint8_t GRenderWindow::processFrameData(const Layout::FramebufferLayout& layout, char *frameData, const QString& address) {
    static DECLJPEGOUTBUF(outBuf);
    static unsigned long outSize = 0;
    static DECLJPEGOUTBUF(outDiffBuf);
    static unsigned long outDiffSize = 0;
    static int forceFrame = 0;
    static int lastFrameWasFull = 0;

    static QTcpSocket sock;
    static unsigned int waitConnection = 0;

    if (sock.state() != QAbstractSocket::ConnectedState) {
        if (!waitConnection) {
            waitConnection = 300;
            sock.connectToHost(address, PORT);
            sock.waitForConnected(1000);
        }
        else waitConnection--;
    }

    if (!frameData) {
        return readConfirmation(sock);
    }

    int width = layout.width;
    int height = layout.height;

    static int checker = 0;

    int16_t numSq = 0;
    if (lastFrameWasFull) forceFrame = 0;
    else if (forceFrame > 100) numSq = -1;

    if(checker) numSq = -1;
    if (numSq != -1) numSq = imageDiff((unsigned char *)frameData, width, height);
    //More than half of the screen? Just send the full frame
    if (numSq > (((240 / 8) * (320 / 8)) / 3)) numSq = -1;
    // int16_t numSq = 0;

    if (numSq == 0) {
        uint16_t dataType = 0;
        socketSend(sock, (const char *)&dataType, 2);
        forceFrame+=2;
    } else {
/*
        const char *jpgBuf = jpegCompress((unsigned char *)frameData, width, height, 40 + (rand()%20), &outBuf, &outSize);
        const char *jpgDiffBuf = 0;

        if (numSq > 0) {
            jpgDiffBuf = jpegCompress((unsigned char *) diffBuf, 8, 8 * numSq, 50 + (rand()%20), &outDiffBuf, &outDiffSize);
        }
*/
        char buf[320 * 240 * 3 / 2];
        char *in = frameData;
        char *out = buf;

        int skip = checker;
        for (int j=0; j<height; j++) {
            for (int i=0; i<width; i++) {
                if (!skip) {
                    out[0] = in[0];
                    out[1] = in[1];
                    out[2] = in[2];
                    out+=3;
                }
                in+=3;
                skip = !skip;
            }
            skip = !skip;
        }
        const char *jpgBuf = jpegCompress((unsigned char *)buf, width/2, height, 70, &outBuf, &outSize);

//        const char *jpgBuf = jpegCompress((unsigned char *)frameData, width, height, 75, &outBuf, &outSize);
        const char *jpgDiffBuf = 0;

        if (numSq > 0) {
            jpgDiffBuf = jpegCompress((unsigned char *) diffBuf, 8, 8 * numSq, 70, &outDiffBuf, &outDiffSize);
        }

        if ((numSq > 0) && ((outDiffSize + sizeof(diffMap)) < outSize)) {
            uint16_t dataType = 2;
            uint16_t dataSize = outDiffSize;
            socketSend(sock, (const char *)&dataType, 2);
            socketSend(sock, (const char *)&dataSize, 2);
            socketSend(sock, (const char *)diffMap, sizeof(diffMap));
            socketSend(sock, jpgDiffBuf, dataSize);
            forceFrame+=5;
            lastFrameWasFull = 0;
        } else {
            uint16_t dataType = 3+checker;
            uint16_t dataSize = outSize;
            socketSend(sock, (const char *)&dataType, 2);
            socketSend(sock, (const char *)&dataSize, 2);
            socketSend(sock, jpgBuf, dataSize);
            if (checker) {
                forceFrame = 0;
                lastFrameWasFull = 1;
            }
            checker = (checker+1) & 1;
            // uint16_t dataType = 1;
            // uint16_t dataSize = outSize;
            // socketSend(sock, (const char *)&dataType, 1);
            // socketSend(sock, (const char *)&dataSize, 2);
            // socketSend(sock, jpgBuf, dataSize);
            // forceFrame = 0;
            // lastFrameWasFull = 1;
        }
    }

    return readConfirmation(sock);
}

void GRenderWindow::ConnectCTroll3D(const QString& address) {
    const Layout::FramebufferLayout layout{Layout::CustomFrameLayout(240, 320)};
    screen_image = QImage(QSize(layout.width, layout.height), QImage::Format_RGB888);

    VideoCore::RequestCTroll3D(
        screen_image.bits(),
        [=](char *frameData)->uint8_t {
            return GRenderWindow::processFrameData(layout, frameData, address);
            // static int hasFuture = 0;
            // static std::future<int> future;
            // int waiting = 1;
            //
            // if (hasFuture) {
            //     std::future_status status = future.wait_for(std::chrono::seconds(0));
            //     if (status == std::future_status::ready) {
            //         waiting = future.get();
            //         future = std::async(std::launch::async, GRenderWindow::processFrameData, layout, frameData, address);
            //     }
            // } else {
            //     waiting = 0;
            //     future = std::async(std::launch::async, GRenderWindow::processFrameData, layout, frameData, address);
            // }
            // hasFuture = 1;
            //
            // return waiting;
        },
        address.toStdString().c_str(),
        layout
    );
}

void GRenderWindow::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    setMinimumSize(minimal_size.first, minimal_size.second);
}

void GRenderWindow::OnEmulationStarting(EmuThread* emu_thread) {
    this->emu_thread = emu_thread;
}

void GRenderWindow::OnEmulationStopping() {
    emu_thread = nullptr;
}

void GRenderWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
}

std::unique_ptr<Frontend::GraphicsContext> GRenderWindow::CreateSharedContext() const {
    return std::make_unique<GLContext>(QOpenGLContext::globalShareContext());
}

GLContext::GLContext(QOpenGLContext* shared_context)
    : context(new QOpenGLContext(shared_context->parent())),
      surface(new QOffscreenSurface(nullptr)) {

    // disable vsync for any shared contexts
    auto format = shared_context->format();
    format.setSwapInterval(0);

    context->setShareContext(shared_context);
    context->setFormat(format);
    context->create();
    surface->setParent(shared_context->parent());
    surface->setFormat(format);
    surface->create();
}

void GLContext::MakeCurrent() {
    context->makeCurrent(surface);
}

void GLContext::DoneCurrent() {
    context->doneCurrent();
}
