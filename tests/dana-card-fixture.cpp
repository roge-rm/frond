#include "SdImage.h"
#include <fstream>
int main(int argc,char**argv){auto bytes=frondSd::create(32);std::ofstream f(argv[1],std::ios::binary);f.write((char*)bytes.data(),bytes.size());return !f;}
