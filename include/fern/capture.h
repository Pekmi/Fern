#include <d3d11.h>
#include <dxgi1_2.h>


#pragma region getters

struct D3D11Context {ID3D11Device* device; ID3D11DeviceContext* deviceContext;};
D3D11Context GetD3D11Device();
IDXGIFactory1* getFactory1();
std::vector<IDXGIAdapter1*> getAdapters1(IDXGIFactory1* factory);
std::vector<IDXGIOutput1*> getOutputs1(IDXGIAdapter1* adapter);
IDXGIOutputDuplication* getOutputDuplication(ID3D11Device* device, IDXGIOutput1* output1);


#pragma region capture

IDXGIResource* getResource(IDXGIOutputDuplication* outputDuplication, UINT timeout = 0);


#pragma region decodage

void decodeResource(IDXGIResource* resource);
ID3D11Texture2D* resourceToTexture(IDXGIResource* resource);
ID3D11Texture2D* createStagingTexture(ID3D11Device* device, ID3D11Texture2D* texture);
void MapAndPrintPixel(ID3D11DeviceContext* context, ID3D11Texture2D* stagingTexture);