REV_NAME="citra-portable"

mkdir "$REV_NAME"

cp build/bin/Release/citra "$REV_NAME"
cp -r build/bin/Release/citra-qt.app "$REV_NAME"
cp build/bin/Release/citra-room "$REV_NAME"

macpack "${REV_NAME}/citra-qt.app/Contents/MacOS/citra-qt" -d "../Frameworks"

$(brew --prefix)/opt/qt5/bin/macdeployqt "${REV_NAME}/citra-qt.app" -executable="${REV_NAME}/citra-qt.app/Contents/MacOS/citra-qt"

macpack "${REV_NAME}/citra" -d "libs"

install_name_tool -change @loader_path/../Frameworks/libc++.1.0.dylib /usr/lib/libc++.1.dylib "${REV_NAME}/citra-qt.app/Contents/MacOS/citra-qt"
install_name_tool -change @loader_path/libs/libc++.1.0.dylib /usr/lib/libc++.1.dylib "${REV_NAME}/citra"

chmod +x ${REV_NAME}/citra-qt.app/Contents/MacOS/citra-qt

