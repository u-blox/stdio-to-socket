# Introduction
This Windows tool provides a mechanism to wrap a given executable such that `stdout` from that executable is redirected to a TCP socket.  It is intended for use with the [test regime](https://github.com/u-blox/infinite-iot/tree/master/TESTS) of the [infinite-iot](https://github.com/u-blox/infinite-iot) project.

The tool can be built with MS Visual Studio C++ (2017).  For usage instructions, run the executable without any parameters.