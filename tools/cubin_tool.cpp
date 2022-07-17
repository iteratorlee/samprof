#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <regex>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <unordered_set>
#include <unordered_map>

#include "tools.h"
#include "get_cubin_crc.h"

#define DEBUG false
#define VERBOSE false
#define PRINTGRAPH false
#define CMD_RESULT_BUF_SIZE 1024

class CubinHelper {
public:
    std::string nvdisasmPath;
    CubinHelper() { nvdisasmPath = "nvdisasm"; }

    CubinHelper(std::string _nvdisasmPath) : nvdisasmPath(_nvdisasmPath) {};

    uint64_t GetCubinCrc(std::string cubinFilePath) {        
        std::ifstream fileHandler(cubinFilePath, std::ios::binary | std::ios::ate);

        if (!fileHandler) {
            std::cerr << "can not find " << cubinFilePath << std::endl;
            return 0;
        }

        size_t cubinSize = fileHandler.tellg();
        if (!fileHandler.seekg(0, std::ios::beg)) {
            std::cerr << "Unable to find size for cubin file " << cubinFilePath << std::endl;
            return 0;
        }

        void* cubinImage = malloc(sizeof(char) * cubinSize);
        fileHandler.read((char*)cubinImage, cubinSize);
        fileHandler.close();

        return GetModuleCubinCrc(cubinSize, cubinImage);
    }

    int GetCubinSASS(std::string cubinFilePath, std::string& result) {
        const std::string cmd = this->nvdisasmPath + " " + cubinFilePath;
        return Cmd(cmd, result);
    }

    int GetCubinCG(std::string cubinFilePath, std::string& result) {
        const std::string cmd = this->nvdisasmPath + " -cfg " + cubinFilePath;
        return Cmd(cmd, result);
    }

private:
    int Cmd(const std::string cmd, std::string& result) {
        int returnCode = -1;
        char buf[CMD_RESULT_BUF_SIZE];
        FILE* ptr;
        
        if ((ptr = popen(cmd.c_str(), "r")) != nullptr) {
            while (fgets(buf, sizeof(buf), ptr) != nullptr) {
                result.append(buf);
            }
            pclose(ptr);
            ptr = nullptr;
            returnCode = 0;
        } else {
            std::cerr << "popen " << cmd << " error" << std::endl;
            returnCode = -1;
        }

        return returnCode;
    }
};

int GetFileofPath(std::string path, std::vector<std::string>& filePath) {
    DIR* pDir;
    struct dirent* ptr;

    if(!(pDir = opendir(path.c_str()))) {
        std::cerr << "path " << path << " does not exist" << std::endl;
        return -1;
    }

    while ((ptr = readdir(pDir)) != 0) {
        if (strcmp(ptr->d_name, ".") != 0 && strcmp(ptr->d_name, "..") != 0) {
            if (path[path.length() - 1] != '/') {
                filePath.push_back(path + "/" + ptr->d_name);
            } else {
                filePath.push_back(path + ptr->d_name);
            }
        }
    }

    return 0;
}

static void ParseInstruction(std::string, Instruction*& instruction) {
    std::string inst = instruction->inst;

}

int FillCG(uint64_t cubinCrc, std::string& cubinSASS, GPUCallingGraph*& graph) {
    std::istringstream ss(cubinSASS + '\n');
    std::string line;

    std::string newLine("\n");
    // comments of new text section for a function
    // example: "//----- .text.func ----"
    std::regex newFunctionReg("^(//-+) \\.text\\.(.+) (-+)$");

    // new text section for a weak (inline) function
    // example: "    .weak    $func$__subfunc"
    std::regex newWeakFunctionReg("^(\\s+)\\.weak(\\s+)(.+)$");

    // label of function
    // example: ".text.func:"
    std::regex functionBeginLabelReg("^\\.text\\.(.+):$");

    // label of weak (inline) function
    // exmple: "$func$__subfunc:"
    std::regex weakFunctionBeginLabelReg("^\\$(.+)\\$(.+):$");

    // common label
    // example: ".L_x_11:"
    //std::regex labelReg("^\\.L_\\w_\\d+:$");
    std::regex labelReg("^(\\S+):$");

    // function size (begin and end labels)
    // example: "    .size    func,(.L_x_11 - func)"
    std::regex functionSizeReg("^(\\s+)\\.size(\\s+)(\\S+),\\((.+) - (.+)\\)$");

    // instruction
    // example: "    /*08a0*/    MOV R4, R3 ;"
    std::regex instructionReg("^(\\s+)\\/\\*([\\d|\\w]+)\\*\\/([\\s|\\{]+)(.+)(\\s?);$");

    // call instruction
    // example: "CALL.ABS.NOINC `(__cuda_sm70_warpsync)"
    std::regex callInstructionReg("CALL\\..+(\\s+)`\\((.+)\\)(\\s?)$");

    // alignment instruction
    // example: "   .align  128"
    std::regex alignReg("^(\\s+)\\.align(\\s+)(\\d+)$");

    // parsing the text section or not
    bool inTextSection = false;
    // function size has been detected or not, used to handle weak function size
    bool isMainFunctionSize = true;
    // alignment size
    uint32_t alignSize = 0;
    // the label parsed just now
    std::unordered_set<std::string> currentLabels;
    // the end label of a function
    std::string functionEndLabel;
    // the function being parsed now
    std::string currentFunctionName;
    // the address of the instruction just parsed
    uint64_t lastAddress;
    // all the functions inside a CUDA module
    FuncMap_t functionMap;

    while (std::getline(ss, line)) {
        std::smatch results;
        if (std::regex_match(line, results, newFunctionReg)) {
            currentFunctionName = results[2];
            if (functionMap.find(currentFunctionName) == functionMap.end()) {
                inTextSection = true;
                functionMap.insert(std::make_pair(currentFunctionName, new Function(
                    currentFunctionName
                )));
            } else {
                std::cerr << "duplicate function name" << std::endl;
                return DUP_FUNCTION_NAME;
            }
            continue;
        }
        
        if (!inTextSection) continue;

        if (std::regex_match(line, results, alignReg)) {
            alignSize = std::strtoul(results[3].str().c_str(), NULL, 10);
        }

        if (std::regex_match(line, results, labelReg)) {
            currentLabels.insert(results[1]);
            continue;
        }

        // handling weak function case 1
        if (std::regex_match(line, results, newWeakFunctionReg)) {
            std::string weakFunctionName = results[3];
            if (functionMap.find(weakFunctionName) == functionMap.end()) {
                functionMap.insert(std::make_pair(weakFunctionName, new Function(
                    weakFunctionName, true, currentFunctionName
                )));
            }
        }

        // handling weak function case 2
        if (std::regex_match(line, results, weakFunctionBeginLabelReg)) {
            std::string weakFunctionName = "$" + results[1].str() + "$" + results[2].str();
            if (functionMap.find(weakFunctionName) == functionMap.end()) {
                functionMap.insert(std::make_pair(weakFunctionName, new Function(
                    weakFunctionName, true, currentFunctionName
                )));
            }
        }

        // handling weak function case 3
        if (std::regex_match(line, results, functionSizeReg)) {
            std::string endLabel = results[4];
            if (isMainFunctionSize) {
                functionEndLabel = endLabel;
                functionMap[currentFunctionName]->SetFunctionEndLabel(endLabel);
                isMainFunctionSize = false;
            } else {
                std::string weakFunctionName = results[3];
                if (functionMap.find(weakFunctionName) == functionMap.end()) {
                    functionMap.insert(std::make_pair(weakFunctionName, new Function(
                        weakFunctionName, true, currentFunctionName
                    )));
                }
                functionMap[weakFunctionName]->SetFunctionEndLabel(endLabel);
            }
        }

        // handling instruction
        if (std::regex_search(line, results, instructionReg)) {
            Instruction *instruction = new Instruction();
            instruction->address = std::strtoull(results[2].str().c_str(), NULL, 16);
            instruction->inst = results[4];
            functionMap[currentFunctionName]->instructions.insert(
                std::make_pair(instruction->address, instruction)
            );

            // instruction referred by labels
            if (currentLabels.size() > 0) {
                // insert label map
                for (auto label: currentLabels) {
                    functionMap[currentFunctionName]->labelMap.insert(
                        std::make_pair(label, instruction)
                    );
                }
                currentLabels.clear();
            }

            // address to handle empty labels
            lastAddress = instruction->address;
        } else if (currentLabels.find(functionEndLabel) != currentLabels.end()) {
            Instruction *instruction = new Instruction();
            instruction->address = lastAddress + alignSize;
            instruction->inst = "null";
            functionMap[currentFunctionName]->instructions.insert(
                std::make_pair(instruction->address, instruction)
            );
            
            for (auto label: currentLabels) {
                functionMap[currentFunctionName]->labelMap.insert(
                    std::make_pair(label, instruction)
                );
            }
            currentLabels.clear();
            
            inTextSection = false;
            isMainFunctionSize = true;
        }
    }

    // DEBUG: print functions
    #if VERBOSE
    for (auto function: functionMap) {
        std::cout << "||======= " << function.first << " =======||" << std::endl;
        for (auto label: function.second->labelMap) {
            std::cout << "label: " << label.first << ": " << label.second->address << std::endl;
        }
        for (auto instruction: function.second->instructions) {
            std::cout << instruction.first << " --> " << instruction.second->inst << std::endl;
        }
        std::cout << std::endl;
    }
    #endif

    for (auto function: functionMap) {
        GPUCallingGraphNode* node = graph->add_nodes();
        node->set_cubincrc(cubinCrc);
        node->set_funcname(function.first);
        node->set_weight(1);

        uint64_t addrBegin = 0;
        if (function.second->weak) {
            std::string weakFucntionName = function.second->functionName;
            auto parentFunction = functionMap[function.second->parentFunctionName];
            if (parentFunction->labelMap.find(weakFucntionName) != parentFunction->labelMap.end()) {
                addrBegin = parentFunction->labelMap[weakFucntionName]->address;
            }
        }
        node->set_addrbegin(addrBegin);

        uint64_t addrEnd;
        if (!function.second->weak){
            addrEnd = function.second->labelMap[function.second->functionEndLabel]->address;
        } else {
            auto parentFunction = functionMap[function.second->parentFunctionName];
            addrEnd = parentFunction->labelMap[function.second->functionEndLabel]->address;
        }
        node->set_addrend(addrEnd);
    }

    for (auto function: functionMap) {
        for (auto instruction: function.second->instructions) {
            std::string inst = instruction.second->inst;
            std::smatch results;
            if (std::regex_match(inst, results, callInstructionReg)) {
                std::string dstFuncName = results[2];
                if (functionMap.find(dstFuncName) != functionMap.end()){
                    std::string srcFuncName = function.first;
                    GPUCallingGraphEdge* edge = graph->add_edges();
                    edge->set_srcfuncname(srcFuncName);
                    edge->set_dstfuncname(dstFuncName);
                    edge->set_srcpcoffset(instruction.first);
                    edge->set_dstpcoffset(0);
                    edge->set_weight(1);
                }
            }
        }
    }

    // free space allocated for tmp functions & instructions
    for (auto function: functionMap) {
        for (auto instruction: function.second->instructions) {
            delete instruction.second;
        }
        delete function.second;
    }

    return PARSE_SUCCESS;
}

int main(int argc, char** argv) {
    #if PRINTGRAPH
    std::string cgPath = argv[1];

    std::vector<std::string> cgFilePath;
    GetFileofPath(cgPath, cgFilePath);

    for (std::string fp: cgFilePath) {
        if (fp.find(".pb.gz") == std::string::npos) continue;
        GPUCallingGraph *graph = new GPUCallingGraph();
        LoadGraphFromFile(graph, fp);
        std::cout << fp; //<< ":" << graph->nodes().size() << std::endl;
        PrintGraph(graph);
    }
    exit(0);
    #endif

    CubinHelper cubinHelper;
    std::string cubinPath = argv[1];
    std::string graphPath = argv[2];

    if (access(cubinPath.c_str(), F_OK)) {
        std::cerr << "cubin path " << cubinPath << " does not exist" << std::endl;
        exit(-1);
    }

    std::vector<std::string> cubinFilePath;
    std::vector<std::string> graphFilePath;
    std::unordered_set<std::string> graphFileSet;
    GetFileofPath(cubinPath, cubinFilePath);
    GetFileofPath(graphPath, graphFilePath);
    for (auto s: graphFilePath) {
        graphFileSet.insert(s);
    }

    int cnt = 1;
    for (std::string fp: cubinFilePath) {
        if (fp.find(".cubin") == std::string::npos) continue;
        uint64_t cubinCrc = cubinHelper.GetCubinCrc(fp);
        #if DEBUG
        if (cubinCrc != 1074760511) continue; // debug
        #endif
        std::string cgPath = graphPath[graphPath.length() - 1] == '/' ? \
            graphPath + std::to_string(cubinCrc) + ".pb.gz" \
            :graphPath + '/' + std::to_string(cubinCrc) + ".pb.gz";
        if (graphFileSet.find(cgPath) != graphFileSet.end()) {
            std::cout << "cg file " << cgPath << " already exist, continue" << std::endl;
            ++cnt;
            continue;
        }
        std::string cubinSASS;
        if (cubinHelper.GetCubinSASS(fp, cubinSASS) != 0) {
            std::cerr << "unable to get sass of " << fp << std::endl;
            continue;
        }
        std::cout << "processing #" << cnt << "/" << cubinFilePath.size() << " " <<\
            fp << ":crc=" << cubinCrc << std::endl;
        GPUCallingGraph *graph = new GPUCallingGraph();
        FillCG(cubinCrc, cubinSASS, graph);
        #if VERBOSE
        PrintGraph(graph);
        #endif
        DumpGraph2File(graph, cgPath);
        std::cout << " dumped to " << cgPath << std::endl;
        ++cnt;
        delete graph;
    }

    return 0;
}