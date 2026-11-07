#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto file = std::filesystem::path(argv[1]);
    Microsoft::WRL::ComPtr<ID3DBlob> shader, errors;
    HRESULT hr = D3DCompileFromFile(file.c_str(), nullptr,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "CSMain", "cs_5_1",
                                    D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                    0, &shader, &errors);
    if (errors) std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                                static_cast<std::streamsize>(errors->GetBufferSize()));
    if (FAILED(hr)) return 1;
    std::cout << "Shader compiled; bytes=" << shader->GetBufferSize() << '\n';
    return 0;
}
