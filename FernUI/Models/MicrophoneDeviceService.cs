using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace FernUI.Models
{
    public sealed class MicrophoneDeviceInfo
    {
        public MicrophoneDeviceInfo(string id, string name, bool isDefault)
        {
            Id = id;
            Name = name;
            IsDefault = isDefault;
        }

        public string Id { get; }
        public string Name { get; }
        public bool IsDefault { get; }
    }

    public sealed class MicrophoneLevelMeter : IDisposable
    {
        private IMMDevice? _device;
        private IAudioMeterInformation? _meter;

        internal MicrophoneLevelMeter(IMMDevice device, IAudioMeterInformation meter)
        {
            _device = device;
            _meter = meter;
        }

        public float GetPeakValue()
        {
            if (_meter == null) return 0;

            int hr = _meter.GetPeakValue(out float peak);
            if (hr < 0) return 0;

            return Math.Clamp(peak, 0.0f, 1.0f);
        }

        public void Dispose()
        {
            if (_meter != null)
            {
                Marshal.ReleaseComObject(_meter);
                _meter = null;
            }

            if (_device != null)
            {
                Marshal.ReleaseComObject(_device);
                _device = null;
            }
        }
    }

    public static class MicrophoneDeviceService
    {
        private static readonly Guid MMDeviceEnumeratorClsid = new("bcde0395-e52f-467c-8e3d-c4579291692e");

        public static IReadOnlyList<MicrophoneDeviceInfo> GetCaptureDevices()
        {
            string defaultId = GetDefaultCaptureDeviceId();
            var microphones = new List<MicrophoneDeviceInfo>();

            IMMDeviceEnumerator? enumerator = null;
            IMMDeviceCollection? collection = null;

            try
            {
                enumerator = CreateDeviceEnumerator();
                int hr = enumerator.EnumAudioEndpoints(EDataFlow.Capture, DeviceState.Active, out collection);
                if (hr < 0 || collection == null) return microphones;

                hr = collection.GetCount(out uint count);
                if (hr < 0) return microphones;

                for (uint i = 0; i < count; i++)
                {
                    IMMDevice? device = null;
                    try
                    {
                        hr = collection.Item(i, out device);
                        if (hr < 0 || device == null) continue;

                        string id = GetDeviceId(device);
                        if (string.IsNullOrWhiteSpace(id)) continue;

                        string name = GetDeviceFriendlyName(device);
                        if (string.IsNullOrWhiteSpace(name)) name = id;

                        bool isDefault = string.Equals(id, defaultId, StringComparison.OrdinalIgnoreCase);
                        microphones.Add(new MicrophoneDeviceInfo(id, name, isDefault));
                    }
                    finally
                    {
                        if (device != null) Marshal.ReleaseComObject(device);
                    }
                }
            }
            finally
            {
                if (collection != null) Marshal.ReleaseComObject(collection);
                if (enumerator != null) Marshal.ReleaseComObject(enumerator);
            }

            return microphones
                .OrderBy(device => !device.IsDefault)
                .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        public static string GetDefaultCaptureDeviceId()
        {
            IMMDeviceEnumerator? enumerator = null;
            IMMDevice? device = null;

            try
            {
                enumerator = CreateDeviceEnumerator();
                int hr = enumerator.GetDefaultAudioEndpoint(EDataFlow.Capture, ERole.Console, out device);
                if (hr < 0 || device == null) return "";

                return GetDeviceId(device);
            }
            finally
            {
                if (device != null) Marshal.ReleaseComObject(device);
                if (enumerator != null) Marshal.ReleaseComObject(enumerator);
            }
        }

        public static MicrophoneLevelMeter CreateLevelMeter(string deviceId)
        {
            if (string.IsNullOrWhiteSpace(deviceId))
            {
                throw new ArgumentException("Aucun micro selectionne.", nameof(deviceId));
            }

            IMMDeviceEnumerator? enumerator = null;
            IMMDevice? device = null;
            bool deviceTransferred = false;

            try
            {
                enumerator = CreateDeviceEnumerator();
                int hr = enumerator.GetDevice(deviceId, out device);
                if (hr < 0 || device == null)
                {
                    Marshal.ThrowExceptionForHR(hr);
                    throw new InvalidOperationException("Micro introuvable.");
                }

                Guid meterId = typeof(IAudioMeterInformation).GUID;
                hr = device.Activate(ref meterId, ClsCtx.All, IntPtr.Zero, out object meterObject);
                if (hr < 0 || meterObject is not IAudioMeterInformation meter)
                {
                    Marshal.ThrowExceptionForHR(hr);
                    throw new InvalidOperationException("Vumetre micro indisponible.");
                }

                deviceTransferred = true;
                return new MicrophoneLevelMeter(device, meter);
            }
            finally
            {
                if (!deviceTransferred && device != null) Marshal.ReleaseComObject(device);
                if (enumerator != null) Marshal.ReleaseComObject(enumerator);
            }
        }

        private static string GetDeviceId(IMMDevice device)
        {
            int hr = device.GetId(out string id);
            return hr < 0 ? "" : id;
        }

        private static IMMDeviceEnumerator CreateDeviceEnumerator()
        {
            Type enumeratorType = Type.GetTypeFromCLSID(MMDeviceEnumeratorClsid, throwOnError: true)
                ?? throw new InvalidOperationException("MMDeviceEnumerator indisponible.");

            object instance = Activator.CreateInstance(enumeratorType)
                ?? throw new InvalidOperationException("MMDeviceEnumerator indisponible.");

            return (IMMDeviceEnumerator)instance;
        }

        private static string GetDeviceFriendlyName(IMMDevice device)
        {
            IPropertyStore? propertyStore = null;
            PropVariant value = default;

            try
            {
                int hr = device.OpenPropertyStore(StorageAccess.Read, out propertyStore);
                if (hr < 0 || propertyStore == null) return "";

                PropertyKey key = PropertyKeys.DeviceFriendlyName;
                hr = propertyStore.GetValue(ref key, out value);
                if (hr < 0) return "";

                return value.GetString() ?? "";
            }
            finally
            {
                if (value.VariantType != 0) PropVariantClear(ref value);
                if (propertyStore != null) Marshal.ReleaseComObject(propertyStore);
            }
        }

        [DllImport("ole32.dll")]
        private static extern int PropVariantClear(ref PropVariant propVariant);
    }

    internal static class PropertyKeys
    {
        public static readonly PropertyKey DeviceFriendlyName = new()
        {
            FormatId = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"),
            PropertyId = 14
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey
    {
        public Guid FormatId;
        public int PropertyId;
    }

    [StructLayout(LayoutKind.Explicit)]
    internal struct PropVariant
    {
        private const ushort VT_BSTR = 8;
        private const ushort VT_LPWSTR = 31;

        [FieldOffset(0)]
        public ushort VariantType;

        [FieldOffset(8)]
        private IntPtr _pointerValue;

        public string? GetString()
        {
            return VariantType switch
            {
                VT_BSTR when _pointerValue != IntPtr.Zero => Marshal.PtrToStringBSTR(_pointerValue),
                VT_LPWSTR when _pointerValue != IntPtr.Zero => Marshal.PtrToStringUni(_pointerValue),
                _ => null
            };
        }
    }

    internal enum EDataFlow
    {
        Render = 0,
        Capture = 1,
        All = 2
    }

    internal enum ERole
    {
        Console = 0,
        Multimedia = 1,
        Communications = 2
    }

    [Flags]
    internal enum DeviceState
    {
        Active = 0x1
    }

    internal enum StorageAccess
    {
        Read = 0
    }

    [Flags]
    internal enum ClsCtx
    {
        All = 23
    }

    [ComImport]
    [Guid("a95664d2-9614-4f35-a746-de8db63617e6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(EDataFlow dataFlow, DeviceState stateMask, out IMMDeviceCollection devices);

        [PreserveSig]
        int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);

        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);

        [PreserveSig]
        int RegisterEndpointNotificationCallback(IntPtr client);

        [PreserveSig]
        int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport]
    [Guid("0bd7a1be-7a1a-44db-8397-cc5392387b5e")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out uint count);

        [PreserveSig]
        int Item(uint index, out IMMDevice device);
    }

    [ComImport]
    [Guid("d666063f-1587-4e43-81f1-b948e807363f")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        [PreserveSig]
        int Activate(ref Guid iid, ClsCtx clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object interfacePointer);

        [PreserveSig]
        int OpenPropertyStore(StorageAccess access, out IPropertyStore properties);

        [PreserveSig]
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);

        [PreserveSig]
        int GetState(out DeviceState state);
    }

    [ComImport]
    [Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IPropertyStore
    {
        [PreserveSig]
        int GetCount(out uint propertyCount);

        [PreserveSig]
        int GetAt(uint propertyIndex, out PropertyKey key);

        [PreserveSig]
        int GetValue(ref PropertyKey key, out PropVariant value);

        [PreserveSig]
        int SetValue(ref PropertyKey key, ref PropVariant value);

        [PreserveSig]
        int Commit();
    }

    [ComImport]
    [Guid("c02216f6-8c67-4b5b-9d00-d008e73e0064")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioMeterInformation
    {
        [PreserveSig]
        int GetPeakValue(out float peak);

        [PreserveSig]
        int GetMeteringChannelCount(out uint channelCount);

        [PreserveSig]
        int GetChannelsPeakValues(uint channelCount, [Out] float[] peakValues);

        [PreserveSig]
        int QueryHardwareSupport(out uint hardwareSupportMask);
    }
}
