#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#pragma region getters

struct D3D11Context {ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> deviceContext;};
D3D11Context GetD3D11Device();
ComPtr<IDXGIFactory1> getFactory1();
std::vector<ComPtr<IDXGIAdapter1>> getAdapters1(IDXGIFactory1* factory);
std::vector<ComPtr<IDXGIOutput1>> getOutputs1(IDXGIAdapter1* adapter);
ComPtr<IDXGIOutputDuplication> getOutputDuplication(ID3D11Device* device, IDXGIOutput1* output1);


#pragma region capture

ComPtr<IDXGIResource> getResource(IDXGIOutputDuplication* outputDuplication, UINT timeout = 0);


#pragma region decodage

void decodeResource(IDXGIResource* resource);
ComPtr<ID3D11Texture2D> resourceToTexture(IDXGIResource* resource);
ComPtr<ID3D11Texture2D> createStagingTexture(ID3D11Device* device, ID3D11Texture2D* texture);
void MapAndPrintPixel(ID3D11DeviceContext* context, ID3D11Texture2D* stagingTexture);