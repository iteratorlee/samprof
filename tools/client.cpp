#include "tools.h"

class GPUProfilingClient {
public:
	GPUProfilingClient(std::shared_ptr<Channel> channel)
		:stub_(GPUProfilingService::NewStub(channel)) {}
	std::string IssuePCSampling(uint32_t duration) {
		GPUProfilingRequest request;
		request.set_duration(duration);

		GPUProfilingResponse response;

		ClientContext context;

		Status status = stub_->PerformGPUProfiling(&context, request, &response);

		if (status.ok()) {
			PrintSamplingResults(response);
			DumpSamplingResults(response, "data/test.dat");
			return response.message();
		} else {
			std::cout << status.error_code() << ": " << status.error_message() << std::endl;
			return "RPC failed";
		}
	}

private:
	std::unique_ptr<GPUProfilingService::Stub> stub_;
};

int main(int argc, char** argv) {
	std::string target_str = "localhost:8886";
	uint32_t duration = 2000;
	if (argc > 1) {
		if (argc != 3) {
			std::cerr << "usage: ./client_cpp <address> <duration>" << std::endl;
			exit(-1);
		}
		target_str = argv[1];
		duration = std::strtoul(argv[2], nullptr, 10);
	}
	grpc::ChannelArguments arg;
	arg.SetMaxReceiveMessageSize(1024 * 1024 * 64);

	GPUProfilingClient client(
		grpc::CreateCustomChannel(target_str, grpc::InsecureChannelCredentials(), arg)
	);
	std::string response = client.IssuePCSampling(duration);
	std::cout << "Client received: " << response << std::endl;

	return 0;
 }