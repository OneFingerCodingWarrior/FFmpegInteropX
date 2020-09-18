#pragma once

#include "UncompressedVideoSampleProvider.h"
#include "TexturePool.h"

#include <d3d11.h>
#include <mfidl.h>
#include <DirectXInteropHelper.h>
#include <set>

extern "C"
{
#include <libavutil/hwcontext_d3d11va.h>
}

namespace FFmpegInterop
{
	using namespace Platform;

	ref class D3D11VideoSampleProvider : UncompressedVideoSampleProvider
	{
	internal:

		D3D11VideoSampleProvider(
			FFmpegReader^ reader,
			AVFormatContext* avFormatCtx,
			AVCodecContext* avCodecCtx,
			FFmpegInteropConfig^ config,
			int streamIndex,
			HardwareDecoderStatus hardwareDecoderStatus)
			: UncompressedVideoSampleProvider(reader, avFormatCtx, avCodecCtx, config, streamIndex, hardwareDecoderStatus)
		{
			decoder = DecoderEngine::FFmpegD3D11HardwareDecoder;
		}

		virtual HRESULT CreateBufferFromFrame(IBuffer^* pBuffer, IDirect3DSurface^* surface, AVFrame* avFrame, int64_t& framePts, int64_t& frameDuration) override
		{
			HRESULT hr = S_OK;

			if (avFrame->format == AV_PIX_FMT_D3D11)
			{
				if (!texturePool)
				{
					// init texture pool, fail if we did not get a device ptr
					if (device && deviceContext)
					{
						texturePool = ref new TexturePool(device, 5);
					}
					else
					{
						hr = E_FAIL;
					}
				}

				//cast the AVframe to texture 2D
				auto nativeSurface = reinterpret_cast<ID3D11Texture2D*>(avFrame->data[0]);
				ID3D11Texture2D* copy_tex = nullptr;

				//get copy texture
				if (SUCCEEDED(hr))
				{
					hr = texturePool->GetCopyTexture(nativeSurface, &copy_tex);
				}

				//copy texture data
				if (SUCCEEDED(hr))
				{
					deviceContext->CopySubresourceRegion(copy_tex, 0, 0, 0, 0, nativeSurface, (UINT)(unsigned long long)avFrame->data[1], NULL);
					deviceContext->Flush();
				}

				//create a IDXGISurface from the shared texture
				IDXGISurface* finalSurface = NULL;
				if (SUCCEEDED(hr))
				{
					hr = copy_tex->QueryInterface(&finalSurface);
				}

				//get the IDirect3DSurface pointer
				if (SUCCEEDED(hr))
				{
					*surface = DirectXInteropHelper::GetSurface(finalSurface);
				}

				if (SUCCEEDED(hr))
				{
					ReadFrameProperties(avFrame, framePts);
				}

				SAFE_RELEASE(finalSurface);
				SAFE_RELEASE(copy_tex);
			}
			else
			{
				hr = UncompressedVideoSampleProvider::CreateBufferFromFrame(pBuffer, surface, avFrame, framePts, frameDuration);

				if (decoder != DecoderEngine::FFmpegSoftwareDecoder)
				{
					decoder = DecoderEngine::FFmpegSoftwareDecoder;
					VideoInfo->DecoderEngine = decoder;
				}
			}

			return hr;
		}

		virtual HRESULT SetSampleProperties(MediaStreamSample^ sample) override
		{
			if (sample->Direct3D11Surface)
			{
				samples.insert(reinterpret_cast<IUnknown*>(sample));
				sample->Processed += ref new Windows::Foundation::TypedEventHandler<Windows::Media::Core::MediaStreamSample^, Platform::Object^>(this, &D3D11VideoSampleProvider::OnProcessed);
			}

			return UncompressedVideoSampleProvider::SetSampleProperties(sample);
		};

		void OnProcessed(Windows::Media::Core::MediaStreamSample^ sender, Platform::Object^ args)
		{
			auto unknown = reinterpret_cast<IUnknown*>(sender->Direct3D11Surface);
			IDXGISurface* surface = NULL;
			ID3D11Texture2D* texture = NULL;

			HRESULT hr = DirectXInteropHelper::GetDXGISurface(sender->Direct3D11Surface, &surface);

			if (SUCCEEDED(hr))
			{
				hr = surface->QueryInterface(&texture);
			}

			if (SUCCEEDED(hr))
			{
				texturePool->ReturnTexture(texture);
			}

			samples.erase(reinterpret_cast<IUnknown*>(sender));

			SAFE_RELEASE(surface);
			SAFE_RELEASE(texture);
		}

		static HRESULT InitializeHardwareDeviceContext(MediaStreamSource^ sender, AVBufferRef* avHardwareContext, ID3D11Device** outDevice, ID3D11DeviceContext** outDeviceContext)
		{
			auto unknownMss = reinterpret_cast<IUnknown*>(sender);
			IMFDXGIDeviceManagerSource* surfaceManager = nullptr;
			IMFDXGIDeviceManager* deviceManager = nullptr;
			HANDLE deviceHandle = INVALID_HANDLE_VALUE;
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* deviceContext = nullptr;
			ID3D11VideoDevice* videoDevice = nullptr;
			ID3D11VideoContext* videoContext = nullptr;

			HRESULT hr = unknownMss->QueryInterface(&surfaceManager);

			if (SUCCEEDED(hr)) hr = surfaceManager->GetManager(&deviceManager);
			if (SUCCEEDED(hr)) hr = deviceManager->OpenDeviceHandle(&deviceHandle);
			if (SUCCEEDED(hr)) hr = deviceManager->GetVideoService(deviceHandle, IID_ID3D11Device, (void**)&device);
			if (SUCCEEDED(hr)) device->GetImmediateContext(&deviceContext);
			if (SUCCEEDED(hr)) hr = deviceManager->GetVideoService(deviceHandle, IID_ID3D11VideoDevice, (void**)&videoDevice);
			if (SUCCEEDED(hr)) hr = deviceContext->QueryInterface(&videoContext);

			auto dataBuffer = (AVHWDeviceContext*)avHardwareContext->data;
			auto internalDirectXHwContext = (AVD3D11VADeviceContext*)dataBuffer->hwctx;

			if (SUCCEEDED(hr))
			{
				// give ownership to FFmpeg
				internalDirectXHwContext->device = device;
				internalDirectXHwContext->device_context = deviceContext;
				internalDirectXHwContext->video_device = videoDevice;
				internalDirectXHwContext->video_context = videoContext;
			}
			else
			{
				// release
				SAFE_RELEASE(device);
				SAFE_RELEASE(deviceContext);
				SAFE_RELEASE(videoDevice);
				SAFE_RELEASE(videoContext);
			}

			if (SUCCEEDED(hr))
			{
				// multithread interface seems to be optional
				ID3D10Multithread* multithread;
				device->QueryInterface(&multithread);
				if (multithread)
				{
					multithread->SetMultithreadProtected(TRUE);
					multithread->Release();
				}
			}

			if (SUCCEEDED(hr))
			{
				auto err = av_hwdevice_ctx_init(avHardwareContext);
				if (err)
				{
					hr = E_FAIL;
				}
			}

			if (SUCCEEDED(hr))
			{
				// addref and hand out pointers
				device->AddRef();
				deviceContext->AddRef();
				*outDevice = device;
				*outDeviceContext = deviceContext;
			}

			if (deviceManager)
			{
				deviceManager->CloseDeviceHandle(deviceHandle);
			}

			SAFE_RELEASE(deviceManager);
			SAFE_RELEASE(surfaceManager);

			return hr;
		}

		TexturePool^ texturePool;
		std::set<IUnknown*> samples;

	};
}

