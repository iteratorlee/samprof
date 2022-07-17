#include "get_cubin_crc.h"

#define CUPTI_CALL(call)                                                    \
{                                                                           \
 CUptiResult _status = call;                                                \
 if (_status != CUPTI_SUCCESS)                                              \
    {                                                                       \
     const char* errstr;                                                    \
     cuptiGetResultString(_status, &errstr);                                \
     fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n",   \
             __FILE__,                                                      \
             __LINE__,                                                      \
             #call,                                                         \
             errstr);                                                       \
     exit(-1);                                                              \
    }                                                                       \
}

uint64_t GetModuleCubinCrc(size_t cubinSize, void* cubinImage) {
    CUpti_GetCubinCrcParams cubinCrcParams = {0};
    cubinCrcParams.size = CUpti_GetCubinCrcParamsSize;
    cubinCrcParams.cubinSize = cubinSize;
    cubinCrcParams.cubin = cubinImage;
    
    CUPTI_CALL(cuptiGetCubinCrc(&cubinCrcParams))

    return cubinCrcParams.cubinCrc;
}


// int main(int argc, char** argv) {
//     char* cubinFileName = argv[1];
//     std::ifstream fileHandler(cubinFileName, std::ios::binary | std::ios::ate);

//     if (!fileHandler) {
//         printf("can not find %s\n", cubinFileName);
//         exit(0);
//     }

//     size_t cubinSize = fileHandler.tellg();
//     if (!fileHandler.seekg(0, std::ios::beg)) {
//         std::cerr << "Unable to find size for cubin file " << cubinFileName << std::endl;
//         exit(-1);
//     }

//     void* cubinImage = malloc(sizeof(char) * cubinSize);
//     fileHandler.read((char*)cubinImage, cubinSize);
//     fileHandler.close();

//     printf("%lu\n", GetModuleCubinCrc(cubinSize, cubinImage));

//     return 0;
// }