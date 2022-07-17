#include <stdio.h>
#include <iostream>
#include <fstream>

#include <cupti_pcsampling.h>
#include <cupti_pcsampling_util.h>

uint64_t GetModuleCubinCrc(size_t cubinSize, void* cubinImage);