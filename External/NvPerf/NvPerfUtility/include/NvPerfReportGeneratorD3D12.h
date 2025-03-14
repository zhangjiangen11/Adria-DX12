/*
* Copyright 2014-2025 NVIDIA Corporation.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#pragma once
#include "NvPerfReportGenerator.h"
#include "NvPerfRangeProfilerD3D12.h"

namespace nv { namespace perf { namespace profiler {
    
    class ReportGeneratorD3D12
    {
    protected:
        struct ReportProfiler : public ReportGeneratorStateMachine::IReportProfiler
        {
            RangeProfilerD3D12 rangeProfiler;

            ReportProfiler()
                : rangeProfiler()
            {
            }

            virtual bool IsInSession() const override
            {
                return rangeProfiler.IsInSession();
            }
            virtual bool IsInPass() const override
            {
                return rangeProfiler.IsInPass();
            }
            virtual bool EndSession() override
            {
                return rangeProfiler.EndSession();
            }
            virtual bool EnqueueCounterCollection(const SetConfigParams& config) override
            {
                return rangeProfiler.EnqueueCounterCollection(config);
            }
            virtual bool BeginPass() override
            {
                return rangeProfiler.BeginPass();
            }
            virtual bool EndPass() override
            {
                return rangeProfiler.EndPass();
            }
            virtual bool PushRange(const char* pRangeName) override
            {
                return rangeProfiler.PushRange(pRangeName);
            }
            virtual bool PopRange() override
            {
                return rangeProfiler.PopRange();
            }
            virtual bool DecodeCounters(DecodeResult& decodeResult) override
            {
                return rangeProfiler.DecodeCounters(decodeResult);
            }
            virtual bool AllPassesSubmitted() const override
            {
                return rangeProfiler.AllPassesSubmitted();
            }
        };

    protected:
        ReportProfiler m_reportProfiler;
        ReportGeneratorStateMachine m_stateMachine;

        // When enabled, OnFrameStart() will check whether its argument's ID3D12Device == m_pDevice.
        bool m_enableCommandQueueValidation;
        Microsoft::WRL::ComPtr<ID3D12Device> m_pDevice;
        ReportGeneratorInitStatus m_initStatus;  // the state of InitializeReportGenerator()
        SessionOptions m_defaultSessionOptions;

    protected:
        bool BeginSessionWithOptions(ID3D12CommandQueue* pCommandQueue, const SessionOptions* pSessionOptions = nullptr)
        {
            const SessionOptions& sessionOptions = pSessionOptions ? *pSessionOptions : m_defaultSessionOptions;
            if (!m_reportProfiler.rangeProfiler.BeginSession(pCommandQueue, sessionOptions))
            {
                NV_PERF_LOG_ERR(10, "m_reportProfiler.rangeProfiler.BeginSession failed\n");
                return false;
            }

            return true;
        }

        bool IsCommandQueueValid(ID3D12CommandQueue* pCommandQueue, const char* pFunctionName) const
        {
            if (!m_enableCommandQueueValidation)
            {
                return true;  // when validation is disabled, always assume the CommandQueue is valid
            }

            if (!m_pDevice)
            {
                NV_PERF_LOG_WRN(50, "Cannot validate CommandQueue.  Please call EnableCommandQueueValidation(true) before InitializeReportGenerator().\n");
                return true;  // allow it to proceed unvalidated
            }

            Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
            HRESULT hr = pCommandQueue->GetDevice(IID_PPV_ARGS(&pDevice));
            if (FAILED(hr) || !pDevice)
            {
                NV_PERF_LOG_ERR(10, "pCommandQueue->GetDevice() failed\n");
                return false;
            }

            if (pDevice != m_pDevice)  // object comparison via WRL::ComPtr, not pointer comparison
            {
                NV_PERF_LOG_ERR(10, "The pCommandQueue passed to %s does not match the ID3D12Device passed to InitializeReportGenerator().\n", pFunctionName);
                return false;
            }

            return true;
        }

    public:
        /// RangeCommands is safe to use on any CommandList belonging to the ID3D12Device used for initialization.
        /// RangeCommands perform no operation when called on unsupported or non-NVIDIA devices.
        D3D12RangeCommands rangeCommands;
        /// NVIDIA device identifiers.
        size_t deviceIndex;
        DeviceIdentifiers deviceIdentifiers;
        std::vector<std::string> additionalMetrics;
        ReportOutputOptions outputOptions;

    public:
        ~ReportGeneratorD3D12()
        {
            Reset();
        }

        ReportGeneratorD3D12()
            : m_reportProfiler()
            , m_stateMachine(m_reportProfiler)
            , m_enableCommandQueueValidation(true)
            , m_pDevice()
            , m_initStatus(ReportGeneratorInitStatus::NeverCalled)
            , m_defaultSessionOptions()
            , rangeCommands()
            , deviceIndex()
            , deviceIdentifiers()
            , additionalMetrics()
            , outputOptions()
        {
            m_defaultSessionOptions.maxNumRanges = ReportGeneratorStateMachine::MaxNumRangesDefault;
        }

        ReportGeneratorInitStatus GetInitStatus() const
        {
            return m_initStatus;
        }

        /// Ends all current sessions and frees all internal memory.
        /// This object may be reused by calling InitializeReportGenerator() again.
        /// Does not reset rangeCommands and deviceIdentifiers.
        void Reset()
        {
            if (m_reportProfiler.rangeProfiler.IsInSession())
            {
                const bool endSessionStatus = m_reportProfiler.rangeProfiler.EndSession();
                if (!endSessionStatus)
                {
                    NV_PERF_LOG_ERR(10, "m_reportProfiler.EndSession failed\n");
                }
            }

            m_stateMachine.Reset();

            m_pDevice = nullptr;
            if (m_initStatus != ReportGeneratorInitStatus::NeverCalled)
            {
                m_initStatus = ReportGeneratorInitStatus::Reset;
            }
        }

        /// Initialize this object on the provided ID3D12Device.
        bool InitializeReportGenerator(ID3D12Device* pDevice)
        {
            // Do this first, in case this object is re-initialized on a different device.
            rangeCommands.Initialize(pDevice);

            m_pDevice = nullptr;
            m_initStatus = ReportGeneratorInitStatus::Failed;

            // Can this device be profiled by Nsight Perf SDK?
            if (!nv::perf::D3D12IsNvidiaDevice(pDevice))
            {
                NV_PERF_LOG_ERR(10, "%ls is not an NVIDIA Device\n", D3D12GetDeviceName(pDevice).c_str());
                return false;
            }

            if (!InitializeNvPerf())
            {
                NV_PERF_LOG_ERR(10, "InitializeNvPerf failed\n");
                return false;
            }

            if (!nv::perf::D3D12LoadDriver())
            {
                NV_PERF_LOG_ERR(10, "Could not load driver\n");
                return false;
            }

            if (!nv::perf::profiler::D3D12IsGpuSupported(pDevice))
            {
                NV_PERF_LOG_ERR(10, "GPU is not supported\n");
                return false;
            }

            deviceIndex = D3D12GetNvperfDeviceIndex(pDevice);
            if (deviceIndex == ~size_t(0))
            {
                NV_PERF_LOG_ERR(10, "Unrecognaized device\n");
                return false;
            }

            deviceIdentifiers = GetDeviceIdentifiers(deviceIndex);
            if (!deviceIdentifiers.pChipName)
            {
                NV_PERF_LOG_ERR(10, "Unrecognaized GPU\n");
                return false;
            }

            auto createMetricsEvaluator = [&](std::vector<uint8_t>& scratchBuffer) {
                const size_t scratchBufferSize = nv::perf::D3D12CalculateMetricsEvaluatorScratchBufferSize(deviceIdentifiers.pChipName);
                if (!scratchBufferSize)
                {
                    return (NVPW_MetricsEvaluator*)nullptr;
                }
                scratchBuffer.resize(scratchBufferSize);
                NVPW_MetricsEvaluator* pMetricsEvaluator = nv::perf::D3D12CreateMetricsEvaluator(scratchBuffer.data(), scratchBuffer.size(), deviceIdentifiers.pChipName);
                return pMetricsEvaluator;
            };
			
            auto createRawCounterConfig = [&]() {
                NVPW_RawCounterConfig* pRawCounterConfig = nv::perf::profiler::D3D12CreateRawCounterConfig(deviceIdentifiers.pChipName);
                return pRawCounterConfig;
            };
            if (!m_stateMachine.InitializeReportMetrics(deviceIndex, deviceIdentifiers, createMetricsEvaluator, createRawCounterConfig, additionalMetrics))
            {
                NV_PERF_LOG_ERR(100, "m_stateMachine.InitializeReportMetrics failed\n");
                return false;
            }

            if (m_enableCommandQueueValidation)
            {
                m_pDevice = pDevice;
            }
            m_initStatus = ReportGeneratorInitStatus::Succeeded;

            NV_PERF_LOG_INF(50, "Initialization succeeded\n");

            return true;
        }

        /// Explicitly starts a session.  This allows you to control resource allocation.
        /// Calling this function is optional; by default, OnFrameStart() will start a session if this isn't called.
        /// The session must be explicitly ended by calling Reset().
        /// The pCommandQueue must belong the ID3D12Device passed into InitializeReportGenerator().
        bool BeginSession(ID3D12CommandQueue* pCommandQueue, const SessionOptions* pSessionOptions = nullptr)
        {
            if (m_initStatus != ReportGeneratorInitStatus::Succeeded)
            {
                NV_PERF_LOG_WRN(100, "skipping; the state of InitializeReportGenerator() is %s.\n", ToCString(m_initStatus));
                return false;
            }
            if (!IsCommandQueueValid(pCommandQueue, "BeginSession"))
            {
                return false;
            }

            auto beginSessionFn = [&]() {
                return BeginSessionWithOptions(pCommandQueue, pSessionOptions);
            };
            if (!m_stateMachine.BeginSession(beginSessionFn))
            {
                return false;
            }
            return true;
        }

        /// Automatically starts collecting counters after StartCollectionOnNextFrame().
        /// Call this at the start of each frame.
        /// The pCommandQueue must belong the ID3D12Device passed into InitializeReportGenerator().
        bool OnFrameStart(ID3D12CommandQueue* pCommandQueue)
        {
            if (m_initStatus != ReportGeneratorInitStatus::Succeeded)
            {
                NV_PERF_LOG_WRN(100, "skipping; the state of InitializeReportGenerator() is %s.\n", ToCString(m_initStatus));
                return false;
            }
            if (!IsCommandQueueValid(pCommandQueue, "OnFrameStart"))
            {
                return false;
            }

            auto beginSessionFn = [&]() {
                return BeginSessionWithOptions(pCommandQueue);
            };
            if (!m_stateMachine.OnFrameStart(beginSessionFn))
            {
                return false;
            }
            return true;
        }

        /// Advances the counter-collection state-machine after rendering.
        /// Call this at the end of each frame.
        bool OnFrameEnd()
        {
            if (m_initStatus != ReportGeneratorInitStatus::Succeeded)
            {
                NV_PERF_LOG_WRN(100, "skipping; the state of InitializeReportGenerator() is %s.\n", ToCString(m_initStatus));
                return false;
            }

            if (!m_stateMachine.OnFrameEnd(outputOptions))
            {
                return false;
            }
            return true;
        }

        /// Reports true after StartCollectionOnNextFrame() is called, until the HTML Report has been written to disk.
        /// This state is cleared by OnFrameEnd().
        bool IsCollectingReport() const
        {
            return m_stateMachine.IsCollectingReport();
        }

        const std::string& GetLastReportDirectoryName() const
        {
            return m_stateMachine.GetLastReportDirectoryName();
        }

        /// Enqueues report collection, starting on the next frame.
        bool StartCollectionOnNextFrame()
        {
            if (m_initStatus != ReportGeneratorInitStatus::Succeeded)
            {
                NV_PERF_LOG_WRN(100, "skipping; the state of InitializeReportGenerator() is %s.\n", ToCString(m_initStatus));
                return false;
            }
            return m_stateMachine.StartCollectionOnNextFrame();
        }

        /// Enables a frame-level parent range.
        /// When enabled (non-NULL, non-empty pRangeName), every frame will have a parent range.
        /// This is also convenient for programs that have no CommandList-level ranges.
        /// Pass in NULL or an empty string to disable this behavior.
        /// The pRangeName string is copied by value, and may be modified or freed after this function returns.
        void SetFrameLevelRangeName(const char* pRangeName)
        {
            m_stateMachine.SetFrameLevelRangeName(pRangeName);
        }

        /// Retrieves the current frame-level parent range.  An empty string signifies no parent range.
        const std::string& GetFrameLevelRangeName() const
        {
            return m_stateMachine.GetFrameLevelRangeName();
        }

        /// Sets the number of Push/Pop nesting levels to collect in the report.
        void SetNumNestingLevels(uint16_t numNestingLevels)
        {
            m_stateMachine.SetNumNestingLevels(numNestingLevels);
        }

        /// Retrieves the number of Push/Pop nesting levels being collected in the report.
        uint16_t GetNumNestingLevels() const
        {
            return m_stateMachine.GetNumNestingLevels();
        }

        /// Sets the maximum number of ranges to collect in the report.
        void SetMaxNumRanges(size_t maxNumRanges)
        {
            m_defaultSessionOptions.maxNumRanges = maxNumRanges;
        }

        /// Retrieves the maximum number of ranges to collect in the report.
        size_t GetMaxNumRanges() const
        {
            return m_defaultSessionOptions.maxNumRanges;
        }

        /// Open the report directory in file browser after perf data collection.
        /// The default behavor is false, and can be changed by enviroment variable NV_PERF_OPEN_REPORT_DIR_AFTER_COLLECTION.
        void SetOpenReportDirectoryAfterCollection(bool openReportDirectoryAfterCollection)
        {
            m_stateMachine.SetOpenReportDirectoryAfterCollection(openReportDirectoryAfterCollection);
        }

        /// When enabled, OnFrameStart() will check whether its argument's ID3D12Device
        /// corresponds to the device passed into InitializeReportGenerator().
        void EnableCommandQueueValidation(bool enable = true)
        {
            m_enableCommandQueueValidation = enable;
        }
    };

}}}
