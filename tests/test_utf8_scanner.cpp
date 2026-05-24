// Test splitting — each file includes the relevant test groups from test_all.cpp
// Since CMakeLists lists individual files, we redirect them all to one master file.
// In a real project these would be separate; here we use #include for simplicity.
#include "test_all.cpp"
