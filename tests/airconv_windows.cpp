// Exercise the standalone Windows converter through its real CLI under Wine.
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  char directory[MAX_PATH], input[MAX_PATH];
  if (!GetTempPathA(MAX_PATH, directory) || !GetTempFileNameA(directory, "air", 0, input)) return 1;
  const std::string output = std::string(input) + ".metallib";
  auto finish = [&](bool success) {
    DeleteFileA(input);
    DeleteFileA(output.c_str());
    std::printf("%s: standalone Windows AIR conversion\n", success ? "passed" : "failed");
    return success ? 0 : 1;
  };
  constexpr char source[] = "RWStructuredBuffer<uint> outbuf : register(u0);"
      "[numthreads(1,1,1)] void main() { outbuf[0] = 42; }";
  ID3DBlob *shader = nullptr, *errors = nullptr;
  HRESULT hr = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &shader, &errors);
  if (errors) {
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    errors->Release();
  }
  if (FAILED(hr) || !shader) return finish(false);
  std::ofstream file(input, std::ios::binary);
  file.write(static_cast<const char *>(shader->GetBufferPointer()), shader->GetBufferSize());
  file.close();
  shader->Release();
  if (!file) return finish(false);
  std::string executable = argv[1];
  if (!executable.empty() && executable[0] == '/') executable = "Z:" + executable;
  for (char &c : executable) if (c == '/') c = '\\';
  std::string command = "\"" + executable + "\" \"" + input + "\" -A -o \"" + output + "\"";
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessA(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) return finish(false);
  CloseHandle(process.hThread);
  DWORD code = 1;
  const bool completed = WaitForSingleObject(process.hProcess, 90000) == WAIT_OBJECT_0;
  if (completed) GetExitCodeProcess(process.hProcess, &code);
  else TerminateProcess(process.hProcess, 1);
  CloseHandle(process.hProcess);
  char magic[4]{};
  std::ifstream result(output, std::ios::binary);
  result.read(magic, sizeof(magic));
  const bool valid = completed && code == 0 && result && !std::memcmp(magic, "MTLB", 4);
  result.close();
  return finish(valid);
}
