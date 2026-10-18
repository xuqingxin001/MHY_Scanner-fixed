#pragma once

#include <iostream>
#include <cwctype>

#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// 虚拟显示适配器(UU超级屏/向日葵/MuMu等间接显示驱动IDD)上 DXGI 桌面复制实测返回损坏帧甚至崩溃,
// 直接过滤掉: 这些输出走 GDI 兜底; 仅物理显卡(NVIDIA/AMD/Intel)启用 DXGI
static bool IsVirtualDisplayAdapter(const DXGI_ADAPTER_DESC1& adesc)
{
    WCHAR low[128]{ 0 };
    for (int i = 0; i < 128 && adesc.Description[i] != L'\0'; ++i)
    {
        low[i] = static_cast<WCHAR>(towlower(adesc.Description[i]));
    }
    return wcsstr(low, L"virtual") != nullptr ||
           wcsstr(low, L"idd") != nullptr ||
           wcsstr(low, L"indirect") != nullptr ||
           wcsstr(low, L"remote") != nullptr ||
           wcsstr(low, L"mirror") != nullptr;
}

class ScreenShotDXGI
{
public:
    ScreenShotDXGI() :
        m_Device(nullptr),
        m_DeskDupl(nullptr),
        m_AcquiredDesktopImage(nullptr),
        m_AcquiredDesktopImage_copy(nullptr),
        m_monitorIdx(0)
    {
    }
    ~ScreenShotDXGI()
    {
        ReleaseResources();
    }

    // 拷贝禁止: 持有 COM 指针的类被浅拷贝会 double Release(导致扫码/退出时闪退)
    ScreenShotDXGI(const ScreenShotDXGI&) = delete;
    ScreenShotDXGI& operator=(const ScreenShotDXGI&) = delete;

    // 移动语义: vector 扩容时正确转移 COM 指针, 源对象置空避免二次释放
    ScreenShotDXGI(ScreenShotDXGI&& other) noexcept
        : m_Device(other.m_Device),
          m_DeskDupl(other.m_DeskDupl),
          m_DeskDupl_state(other.m_DeskDupl_state),
          m_monitorIdx(other.m_monitorIdx),
          m_AcquiredDesktopImage(other.m_AcquiredDesktopImage),
          m_AcquiredDesktopImage_copy(other.m_AcquiredDesktopImage_copy),
          m_outputRect(other.m_outputRect),
          selectedAdapter(other.selectedAdapter)
    {
        other.m_Device = nullptr;
        other.m_DeskDupl = nullptr;
        other.m_AcquiredDesktopImage = nullptr;
        other.m_AcquiredDesktopImage_copy = nullptr;
        other.selectedAdapter = nullptr;
        other.m_DeskDupl_state = false;
    }

    ScreenShotDXGI& operator=(ScreenShotDXGI&& other) noexcept
    {
        if (this != &other)
        {
            ReleaseResources();
            m_Device = other.m_Device;
            m_DeskDupl = other.m_DeskDupl;
            m_DeskDupl_state = other.m_DeskDupl_state;
            m_monitorIdx = other.m_monitorIdx;
            m_AcquiredDesktopImage = other.m_AcquiredDesktopImage;
            m_AcquiredDesktopImage_copy = other.m_AcquiredDesktopImage_copy;
            m_outputRect = other.m_outputRect;
            selectedAdapter = other.selectedAdapter;
            other.m_Device = nullptr;
            other.m_DeskDupl = nullptr;
            other.m_AcquiredDesktopImage = nullptr;
            other.m_AcquiredDesktopImage_copy = nullptr;
            other.selectedAdapter = nullptr;
            other.m_DeskDupl_state = false;
        }
        return *this;
    }

private:
    void ReleaseResources()
    {
        if (m_AcquiredDesktopImage)
        {
            m_AcquiredDesktopImage->Release();
            m_AcquiredDesktopImage = nullptr;
        }
        if (m_AcquiredDesktopImage_copy)
        {
            m_AcquiredDesktopImage_copy->Release();
            m_AcquiredDesktopImage_copy = nullptr;
        }

        if (m_DeskDupl)
        {
            m_DeskDupl->Release();
            m_DeskDupl = nullptr;
        }

        if (m_Device)
        {
            m_Device->Release();
            m_Device = nullptr;
        }
    }
public:
    /*
	 * @brief InitDevic
	 * @param 2
	 * @param 3
	 * @return Initialization Result
	 */
    bool InitDevice()
    {
        if (m_Device)
        {
            m_Device->Release();
            m_Device = nullptr;
        }
        ChooseAdapter();

        HRESULT hr{ S_OK };
        // Feature levels supported
        D3D_FEATURE_LEVEL FeatureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_1
        };
        UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

        D3D_FEATURE_LEVEL FeatureLevel;

        // Create device

        hr = D3D11CreateDevice(
            selectedAdapter,
            D3D_DRIVER_TYPE_UNKNOWN, // D3D_DRIVER_TYPE_UNKNOWN
            nullptr,
            /* D3D11_CREATE_DEVICE_BGRA_SUPPORT
		* This flag adds support for surfaces with a different
		* color channel ordering than the API default.
		* You need it for compatibility with Direct2D. */
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            FeatureLevels,
            NumFeatureLevels,
            D3D11_SDK_VERSION,
            &m_Device,
            &FeatureLevel,
            nullptr);
        if (SUCCEEDED(hr))
        {
            //trrlog::Log_debug("InitDevice success");
            return true;
        }
        //trrlog::Log_debug("InitDevice error");
        return false;
    }

    bool InitDupl(UINT monitorIdx, int& duplWidth, int& duplHeight)
    {
        // 释放旧资源, 支持运行中分辨率变化后重新初始化
        if (m_AcquiredDesktopImage)
        {
            m_AcquiredDesktopImage->Release();
            m_AcquiredDesktopImage = nullptr;
        }
        if (m_AcquiredDesktopImage_copy)
        {
            m_AcquiredDesktopImage_copy->Release();
            m_AcquiredDesktopImage_copy = nullptr;
        }
        if (m_DeskDupl)
        {
            m_DeskDupl->Release();
            m_DeskDupl = nullptr;
        }
        if (m_Device)
        {
            m_Device->Release();
            m_Device = nullptr;
        }
        m_DeskDupl_state = false;
        m_monitorIdx = monitorIdx;

        // 跨所有适配器(物理显卡+UU/向日葵/MuMu等虚拟显示适配器)按全局输出序号查找,
        // 找到目标输出后为该适配器创建设备并创建桌面复制
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        {
            return false;
        }
        IDXGIAdapter1* adapter = nullptr;
        UINT globalOut = 0;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
        {
            DXGI_ADAPTER_DESC1 adesc{};
            adapter->GetDesc1(&adesc);
            // 跳过软件渲染与虚拟显示适配器(虚拟屏DXGI坏帧/崩溃, 由GDI兜底)
            if (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE || IsVirtualDisplayAdapter(adesc))
            {
                adapter->Release();
                adapter = nullptr;
                continue;
            }
            // 统计该适配器的输出数量
            UINT outCount = 0;
            IDXGIOutput* tmpOut = nullptr;
            while (adapter->EnumOutputs(outCount, &tmpOut) == S_OK)
            {
                tmpOut->Release();
                tmpOut = nullptr;
                ++outCount;
            }
            if (monitorIdx < globalOut + outCount)
            {
                // 目标输出在本适配器上
                const UINT localIdx = monitorIdx - globalOut;
                ID3D11Device* dev = nullptr;
                D3D_FEATURE_LEVEL levels[] = {
                    D3D_FEATURE_LEVEL_11_0,
                    D3D_FEATURE_LEVEL_10_1,
                    D3D_FEATURE_LEVEL_10_0,
                    D3D_FEATURE_LEVEL_9_1
                };
                D3D_FEATURE_LEVEL fl;
                HRESULT hr = D3D11CreateDevice(
                    adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                    D3D11_SDK_VERSION, &dev, &fl, nullptr);
                if (FAILED(hr))
                {
                    adapter->Release();
                    factory->Release();
                    return false;
                }
                IDXGIDevice* dxgiDev = nullptr;
                hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
                IDXGIAdapter* devAdapter = nullptr;
                if (SUCCEEDED(hr))
                {
                    hr = dxgiDev->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&devAdapter));
                    dxgiDev->Release();
                }
                if (FAILED(hr))
                {
                    dev->Release();
                    adapter->Release();
                    factory->Release();
                    return false;
                }
                IDXGIOutput* output = nullptr;
                hr = devAdapter->EnumOutputs(localIdx, &output);
                if (FAILED(hr))
                {
                    dev->Release();
                    devAdapter->Release();
                    adapter->Release();
                    factory->Release();
                    return false;
                }
                IDXGIOutput1* output1 = nullptr;
                hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
                if (SUCCEEDED(hr))
                {
                    // 记录该输出在虚拟桌面的坐标, 用于与GDI屏幕矩形精确配对
                    DXGI_OUTPUT_DESC odesc{};
                    if (SUCCEEDED(output->GetDesc(&odesc)))
                    {
                        m_outputRect = odesc.DesktopCoordinates;
                    }
                }
                output->Release();
                if (FAILED(hr))
                {
                    dev->Release();
                    devAdapter->Release();
                    adapter->Release();
                    factory->Release();
                    return false;
                }
                IDXGIOutputDuplication* dupl = nullptr;
                hr = output1->DuplicateOutput(dev, &dupl);
                output1->Release();
                if (FAILED(hr))
                {
                    dev->Release();
                    devAdapter->Release();
                    adapter->Release();
                    factory->Release();
                    return false;
                }
                m_Device = dev;
                m_DeskDupl = dupl;
                DXGI_OUTDUPL_DESC outDuplDesc;
                m_DeskDupl->GetDesc(&outDuplDesc);
                duplWidth = outDuplDesc.ModeDesc.Width;
                duplHeight = outDuplDesc.ModeDesc.Height;
                m_DeskDupl_state = true;
                devAdapter->Release();
                adapter->Release();
                factory->Release();
                return true;
            }
            globalOut += outCount;
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
        return false;
    }

    /*
	 * @brief 获取当前正在复制的显示器输出尺寸
	 */
    bool GetOutputSize(int& ow, int& oh)
    {
        if (!m_DeskDupl)
        {
            return false;
        }
        DXGI_OUTDUPL_DESC outDuplDesc;
        m_DeskDupl->GetDesc(&outDuplDesc);
        ow = outDuplDesc.ModeDesc.Width;
        oh = outDuplDesc.ModeDesc.Height;
        return true;
    }

    // 该输出在虚拟桌面坐标系中的矩形(与 GDI EnumDisplayMonitors 的坐标同一体系)
    const RECT& GetOutputRect() const
    {
        return m_outputRect;
    }

    /*
	 * @brief 枚举可用的显示器输出数量(所有适配器: 物理显卡+虚拟显示适配器)
	 */
    static int GetOutputCount()
    {
        IDXGIFactory1* factory{ nullptr };
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        {
            return 0;
        }
        int count = 0;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 adesc{};
            adapter->GetDesc1(&adesc);
            // 只统计物理显卡输出(虚拟显示适配器走GDI, 不参与DXGI序号)
            if (!(adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !IsVirtualDisplayAdapter(adesc))
            {
                IDXGIOutput* output = nullptr;
                for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j)
                {
                    ++count;
                    output->Release();
                    output = nullptr;
                }
            }
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
        return count;
    }

    /*
	 * @brief Get
	 * @param timeout
	 * @return 0
	 * @return 1
	 * @return 2
	 */
    int getFrame(int timeout = 100)
    {
        if (!m_DeskDupl_state)
        {
            //LOGE("Duplication Abnormal, unable to getFrame");
            //return GETFRAME_DUPLICATION_ERROR;
            return 1;
        }
        // If still holding old frame, destroy it
        if (m_AcquiredDesktopImage)
        {
            m_AcquiredDesktopImage->Release();
            m_AcquiredDesktopImage = nullptr;
        }

        IDXGIResource* DesktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO FrameInfo;

        // Get new frame
        HRESULT hr = m_DeskDupl->AcquireNextFrame(timeout, &FrameInfo, &DesktopResource);

        if (FAILED(hr))
        {
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                //屏幕无变化可能引起该错误
                //LOGE("[0x%08X]: AcquireNextFrame failed.(May be caused by timeout)", hr);

                if (DesktopResource)
                {
                    DesktopResource->Release();
                    DesktopResource = nullptr;
                }

                //return GETFRAME_ERROR;
                return 2;
            }
            else
            {
                //切换屏幕，锁屏可能引起该错误，一旦进入该错误，需要重启Duplication才能重新使用
                //char log[100];
                //sprintf_s(log, "[0x%08X]: AcquireNextFrame failed.(May be caused by screen switching)", hr);
                //WRITELOG(log);

                if (DesktopResource)
                {
                    DesktopResource->Release();
                    DesktopResource = nullptr;
                }
                m_DeskDupl_state = false;
                //return GETFRAME_DUPLICATION_ERROR;
                return 1;
            }
        }

        // QI for IDXGIResource
        hr = DesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&m_AcquiredDesktopImage));

        DesktopResource->Release();
        DesktopResource = nullptr;

        if (FAILED(hr))
        {
            return 2;
        }
        //return GETFRAME_SUCCESS;
        return 0;
    }

    bool copyFrameToBuffer(BYTE** buffer, long bufferSize)
    {
        if (!m_AcquiredDesktopImage)
        {
            return false; // 无有效帧时直接返回, 防止空指针崩溃
        }
        HRESULT hr;
        ID3D11DeviceContext* context;
        m_Device->GetImmediateContext(&context);

        if (!m_AcquiredDesktopImage_copy)
        {
            D3D11_TEXTURE2D_DESC desc;
            m_AcquiredDesktopImage->GetDesc(&desc);
            // Create CPU access texture m_AcquiredDesktopImage_copy
            D3D11_TEXTURE2D_DESC copyImageDesc{};
            copyImageDesc.Width = desc.Width;
            copyImageDesc.Height = desc.Height;
            copyImageDesc.Format = desc.Format;
            copyImageDesc.ArraySize = 1;
            copyImageDesc.BindFlags = 0;
            copyImageDesc.MiscFlags = 0;
            copyImageDesc.SampleDesc.Count = 1;
            copyImageDesc.SampleDesc.Quality = 0;
            copyImageDesc.MipLevels = 1;
            copyImageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            copyImageDesc.Usage = D3D11_USAGE_STAGING;
            hr = m_Device->CreateTexture2D(&copyImageDesc, NULL, &m_AcquiredDesktopImage_copy);

            if (FAILED(hr))
            {
                //LOGE("[0x%08X]: CreateTexture2D failed.", hr);
                return false;
            }
        }

        context->CopyResource(m_AcquiredDesktopImage_copy, m_AcquiredDesktopImage); //源Texture2D和目的Texture2D需要有相同的多重采样计数和质量时

        D3D11_MAPPED_SUBRESOURCE mapRes;
        UINT subresource = D3D11CalcSubresource(0, 0, 0);

        hr = context->Map(m_AcquiredDesktopImage_copy, subresource, D3D11_MAP_READ, 0, &mapRes);
        if (FAILED(hr))
        {
            //LOGE("[0x%08X]: context->Map failed.", hr);
            return false;
        }
        BYTE* dptr = *buffer;
        memcpy_s(dptr, bufferSize, mapRes.pData, bufferSize);
        context->Unmap(m_AcquiredDesktopImage_copy, subresource);
        return true;
    }

    bool doneWithFrame()
    {
        HRESULT hr = m_DeskDupl->ReleaseFrame();
        if (FAILED(hr))
        {
            return false;
        }

        if (m_AcquiredDesktopImage)
        {
            m_AcquiredDesktopImage->Release();
            m_AcquiredDesktopImage = nullptr;
        }

        return true;
    }

private:
    void ChooseAdapter()
    {
        IDXGIFactory1* dxgiFactory{ nullptr };
        HRESULT hr{ CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory) };

        if (FAILED(hr))
        {
            return;
        }
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &selectedAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            hr = selectedAdapter->GetDesc1(&desc);
            char narrowString[100];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrowString, sizeof(narrowString), NULL, NULL);
            //trrlog::Log_debug("{}", narrowString);
            if (desc.Flags != DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                //trrlog::Log_debug("used display adapter{}", narrowString);
                break;
            }
        }
        dxgiFactory->Release();
    }

private:
    ID3D11Device* m_Device;
    IDXGIOutputDuplication* m_DeskDupl;
    bool m_DeskDupl_state = false;

    UINT m_monitorIdx;
    ID3D11Texture2D* m_AcquiredDesktopImage;
    ID3D11Texture2D* m_AcquiredDesktopImage_copy;

    RECT m_outputRect{};
    IDXGIAdapter1* selectedAdapter;
};
