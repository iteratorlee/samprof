#include <iostream>
#include <memory>
#include <string>
#include <queue>
#include <fstream>
#include <map>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "cpp-gen/gpu_profiling.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using gpuprofiling::GPUProfilingService;
using gpuprofiling::GPUProfilingRequest;
using gpuprofiling::GPUProfilingResponse;
using gpuprofiling::CUptiPCSamplingData;
using gpuprofiling::CUptiPCSamplingPCData;
using gpuprofiling::PCSamplingStallReason;
using gpuprofiling::CPUCallingContextNode;
using gpuprofiling::CPUCallingContextTree;
using gpuprofiling::GPUCallingGraphNode;
using gpuprofiling::GPUCallingGraphEdge;
using gpuprofiling::GPUCallingGraph;

typedef enum {
	DUP_FUNCTION_NAME = 0,
	PARSE_SUCCESS = 0x7fffffff
} ParseSassStatus;

static void PrintSamplingResults(GPUProfilingResponse response) {
	std::cout << "pc sampling data size: " << response.pcsamplingdata_size() << std::endl;
	auto pcSamplingData = response.pcsamplingdata();
	uint64_t nPCSamples = 0;

	for (int i = 0; i < response.pcsamplingdata_size(); ++i) {
		CUptiPCSamplingData data = pcSamplingData[i];
		std::cout << "\nthe #" << i << " record" << std::endl;
		std::cout << "size=" << data.size() << std::endl;
		std::cout << "collectNumPcs=" << data.collectnumpcs() << std::endl;
		std::cout << "totalSamples=" << data.totalsamples() << std::endl;
		std::cout << "droppedSamples=" << data.droppedsamples() << std::endl;
		std::cout << "totalNumPcs=" << data.totalnumpcs() << std::endl;
		std::cout << "remainingNumPcs=" << data.remainingnumpcs() << std::endl;
		std::cout << "rangeId=" << data.rangeid() << std::endl;
		std::cout << "nonUsrKernelTotalSamples=" << data.nonusrkernelstotalsamples() << std::endl;

		std::cout << "pcSamplingPCData.size=" << data.ppcdata_size() << std::endl;
		auto pcSamplingPCData = data.ppcdata();
		for (int j = 0; j < data.ppcdata_size(); ++j) {
			CUptiPCSamplingPCData pcData = pcSamplingPCData[j];
			std::cout << "pcData.size=" << pcData.size() << ", " \
						 "cubinCrc=" << pcData.cubincrc() << ", " \
						 "pcOffset=" << pcData.pcoffset() << ", " \
						 "functionIndex=" << pcData.functionindex() << ", " \
						 "functionName=" << pcData.functionname() << ", " \
						 "pad=" << pcData.pad() << ", " \
						 "parentCPUPCId=" << pcData.parentcpupcid() << ", " \
						 "stallReasonCount=" << pcData.stallreasoncount() << std::endl;
			auto stallReason = pcData.stallreason();
			for (int k = 0; k < pcData.stallreasoncount(); ++k) {
				PCSamplingStallReason sReason = stallReason[k];
				std::cout << "		pcSamplingStallReasonsIndex=" << sReason.pcsamplingstallreasonindex() << \
							 "		samples=" << sReason.samples() << std::endl;
				nPCSamples += sReason.samples();
			}
		}
	}

	// printing the cct following a BFS order
	auto CCTs = response.cpucallingctxtree();
	int cctCnt = 1;
	for (auto CCT: CCTs){
		printf("[CCT #%d]\n", cctCnt++);
		if (CCT.nodemap_size()) {
			std::queue<CPUCallingContextNode> nodeQueue;
			nodeQueue.push(CCT.nodemap().at(CCT.rootid()));
			while (!nodeQueue.empty()) {
				CPUCallingContextNode node = nodeQueue.front();
				printf("[CCTNode] id=%lu, pc=%p, offset=%lu, funcName=%s, nchilds=%d, childs=", \
					node.id(), (void *)node.pc(), node.offset(), node.funcname().c_str(), node.childpcs_size());
				for (auto childid: node.childids()) {
					printf("%lu,", childid);
					nodeQueue.push(CCT.nodemap().at(childid));
				}
				printf("\n");
				nodeQueue.pop();
			}
		}
	}

	printf("number of collected GPU pc samples: %lu\n", nPCSamples);
}

static bool DumpSamplingResults(GPUProfilingResponse response, std::string filename) {
	std::ofstream fout;
	fout.open(filename, std::ios::out | std::ios::binary);
	if (fout.is_open()){
		std::cout << "dumping response to " << filename << std::endl;
		return response.SerializeToOstream(&fout);
	} else {
		std::cerr << "can not open " << filename << std::endl;
	}
	return false;
}

static bool LoadSamplingResults(GPUProfilingResponse& response, std::string filename) {
	std::ifstream fin;
	fin.open(filename, std::ios::in | std::ios::binary);
	if (fin.is_open()) {
		return response.ParseFromIstream(&fin);
	}
	return false;
}

bool DumpGraph2File(GPUCallingGraph* graph, std::string filename) {
	std::ofstream fout;
	fout.open(filename, std::ios::out | std::ios::binary);
	if (fout.is_open()) {
		return graph->SerializeToOstream(&fout);
	}
	return false;
}

bool LoadGraphFromFile(GPUCallingGraph*& graph, std::string filename) {
	std::ifstream fin;
	fin.open(filename, std::ios::in | std::ios::binary);
	if (fin.is_open()) {
		return graph->ParseFromIstream(&fin);
	}
	return false;
}

void PrintGraph(GPUCallingGraph* graph) {
	uint64_t totalLoc = 0;
	int n_node = 0;
	for (auto node: graph->nodes()) {
		if (node.funcname().find('$') == std::string::npos){
			++n_node;
			totalLoc += (node.addrend() - node.addrbegin());
		}
	}
	if (n_node == 0) n_node = 1;
	std::cout << ":" << graph->nodes().size() << ":" << n_node << ":" << totalLoc
			  << ":" << totalLoc / n_node << std::endl;
	for (auto node: graph->nodes()) {
		printf("[CG Node] funcName=%s, cubinCrc=%lu, weight=%lu, loc=%lu\n", \
			node.funcname().c_str(), node.cubincrc(), node.weight(), (node.addrend() - node.addrbegin()));
	}
	for (auto edge: graph->edges()) {
		printf("[CG Edge] srcFuncName=%s, srcOffset=%lu, dstFuncName=%s, dstOffset=%lu\n", \
			edge.srcfuncname().c_str(), edge.srcpcoffset(), \
			edge.dstfuncname().c_str(), edge.dstpcoffset());
	}
}

struct Instruction {
	uint64_t address;
	std::string inst;

	bool isCall;
	std::string calleeFunctionName;

	Instruction(): isCall(false) {};
	Instruction(uint64_t _address, std::string _inst): address(_address), inst(_inst) {};
};

typedef std::map<uint64_t, Instruction*> InstMap_t;
typedef std::unordered_map<std::string, Instruction*> LabelMap_t;

struct Function {
	std::string functionName;
	std::string functionEndLabel;
	bool weak;
	std::string parentFunctionName; // valid only if when weak is true;
	InstMap_t instructions;
	LabelMap_t labelMap;

	Function(): weak(false) {};
	Function(std::string _functionName): weak(false), functionName(_functionName) {};
	Function(std::string _functionName, bool _weak, std::string _parentFunctionName): \
		weak(_weak), functionName(_functionName), parentFunctionName(_parentFunctionName) {};

	void SetFunctionEndLabel(std::string _functionEndLabel) {
		functionEndLabel = _functionEndLabel;
	}

	void SetInstructions(InstMap_t& _instructions) {
		instructions = _instructions;
	}

	void SetLabelMap(LabelMap_t _labelMap) {
		labelMap = _labelMap;
	}

	void SetParentFunctionName(std::string _parentFunctionName) {
		parentFunctionName = _parentFunctionName;
	}
};

typedef std::unordered_map<std::string, Function*> FuncMap_t;
