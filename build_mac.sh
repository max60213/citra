cd build
cmake .. -DCMAKE_OSX_ARCHITECTURES="x86_64;x86_64h" -DCMAKE_BUILD_TYPE=Release
make -j8

