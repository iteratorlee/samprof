#ifndef __CCT_INCLUDED__
#define __CCT_INCLUDED__
#include <stdio.h>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>

#include <libunwind.h>

typedef enum {
	// null node
	NULL_NODE = 0,
	// the node already exists
	DUP_NODE = 1,
	// the parent node does not exists
	PARENT_NOT_EXIST = 2,
	// there is only one root node permitted
	DUP_ROOT = 3,
	// the node id has already been used
	DUP_ID = 4,
	// the child nodes of a parent node should have different pcs
	DUP_PC = 5,
	INSERT_SUCCESS = 0x7fffffff
} CallingCtxTreeStatus;

typedef enum {
	CCTNODE_TYPE_CXX = 0,
	CCTNODE_TYPE_PY = 1
} CCTNodeType;

class CPUCCTNode {
public:
	uint64_t id;
	uint64_t pc;
	uint64_t parentID;
	uint64_t parentPC;
	uint64_t offset;
	CCTNodeType nodeType;
	std::string funcName;
	std::vector<CPUCCTNode*> childNodes;
	std::unordered_map<uint64_t, CPUCCTNode*> pc2ChildNodes;
	std::unordered_map<uint64_t, CPUCCTNode*> id2ChildNodes;

	CPUCCTNode(): parentID(0), parentPC(0), nodeType(CCTNODE_TYPE_CXX) {};
	CPUCCTNode(CCTNodeType t): parentID(0), parentPC(0), nodeType(t) {};

	int addChild(CPUCCTNode* child, bool ignoreDupPC=false) {
		childNodes.push_back(child);
		if (id2ChildNodes.find(child->id) != id2ChildNodes.end()) {
			return DUP_ID;
		}
		if (!ignoreDupPC && pc2ChildNodes.find(child->pc) != pc2ChildNodes.end()) {
			return DUP_PC;
		}
		id2ChildNodes.insert(std::make_pair(
			child->id, child
		));
		// TODO insert <pc, child> pair according to parent's pc (offset)
		pc2ChildNodes.insert(std::make_pair(
			child->pc, child
		));
		child->parentID = id;
		child->parentPC = pc;
		return INSERT_SUCCESS;
	}

	// TODO check child by pc (offset) of the parent node
	CPUCCTNode* getChildbyPC(uint64_t pc) {
		if (pc2ChildNodes.find(pc) == pc2ChildNodes.end()) return nullptr;
		return pc2ChildNodes[pc];
	}

	static void copyNodeWithoutRelation(CPUCCTNode* src, CPUCCTNode* dst) {
		dst->id = src->id;
		dst->pc = src->pc;
		dst->offset = src->offset;
		dst->funcName = src->funcName;
		dst->nodeType = src->nodeType;
	}
};

class CPUCCT {
public:
	CPUCCTNode* root;
	std::unordered_map<uint64_t, CPUCCTNode*> nodeMap;

	CPUCCT(): root(nullptr) {};
	CPUCCT(CPUCCTNode* _root): root(_root) {};

	int setRootNode(CPUCCTNode* rootNode) {
		if (root) return DUP_ROOT;
		root = rootNode;
		nodeMap.insert(std::make_pair(root->id, root));
		return INSERT_SUCCESS;
	}

	int insertNode(CPUCCTNode* parent, CPUCCTNode* child, bool ignoreDupPC=false) {
		int insertStatus = parent->addChild(child, ignoreDupPC);
		if (insertStatus != INSERT_SUCCESS) {
			return insertStatus;
		}
		nodeMap.insert(std::make_pair(child->id, child));
		return INSERT_SUCCESS;
	}

	void printTree() {
		std::cout << "************* Begin CCT ***********" << std::endl;
		for (auto itr: nodeMap) {
			std::cout << itr.first << ": pc=" << itr.second->pc << ", parentID=" << itr.second->parentID
					  << ", funcName=" << itr.second->funcName << std::endl;
		}
		std::cout << "************** End CCT ************" << std::endl;
	}
};

typedef std::unordered_map<pthread_t, CPUCCT*> CCTMAP_t;

static bool HasExcludePatterns(std::string funcName) {
	std::vector<std::string> excludePatterns = {"cupti", "CUpti", "cuTexRefGetArray"};
	for (auto pattern: excludePatterns) {
		if(funcName.find(pattern) != std::string::npos) {
			return true;
		}
	}
	return false;
}
#endif