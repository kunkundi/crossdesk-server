#include "signal_server.h"
#include "log.h"

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    InitLogger(std::string(argv[2]) + "/logs");
    SignalServer server(static_cast<uint16_t>(std::stoi(argv[1])), argv[2],
                        std::string(argv[2]) + "/test.db");
    server.Run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
