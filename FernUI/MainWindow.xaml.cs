using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Windows.Foundation;

namespace FernUI
{
    public sealed partial class MainWindow : Window
    {
        private Microsoft.UI.Windowing.AppWindow _appWindow;
        private readonly DispatcherTimer _daemonStatusTimer = new();
        private bool _isCheckingDaemonStatus;
        private bool _isTogglingDaemon;
        private bool? _lastDaemonActive;
        private bool _isContentFullScreen;
        private bool _wasWindowFullScreenBeforeContentFullScreen;

        public MainWindow()
        {
            this.InitializeComponent();
            ConfigureBundledToolsPath();

            // Configurer la taille au démarrage
            IntPtr hWnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            Microsoft.UI.WindowId windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hWnd);
            _appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(windowId);
            _appWindow.Resize(new Windows.Graphics.SizeInt32(1280, 720));

            ExtendsContentIntoTitleBar = true;
            SetTitleBar(null);

            _daemonStatusTimer.Interval = TimeSpan.FromSeconds(2);
            _daemonStatusTimer.Tick += DaemonStatusTimer_Tick;
            _daemonStatusTimer.Start();
            _ = EnsureDaemonStartedAsync();
        }

        private static void ConfigureBundledToolsPath()
        {
            string baseDirectory = AppContext.BaseDirectory;
            string[] candidates =
            {
                Path.Combine(baseDirectory, "..", "tools"),
                Path.Combine(baseDirectory, "tools")
            };

            foreach (string candidate in candidates)
            {
                string toolsPath = Path.GetFullPath(candidate);
                if (!Directory.Exists(toolsPath)) continue;

                string currentPath = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
                bool alreadyPresent = currentPath.Split(Path.PathSeparator)
                    .Any(path => string.Equals(path.Trim(), toolsPath, StringComparison.OrdinalIgnoreCase));

                if (!alreadyPresent)
                {
                    Environment.SetEnvironmentVariable("PATH", toolsPath + Path.PathSeparator + currentPath);
                }

                return;
            }
        }

        private async void CaptureButton_Click(object sender, RoutedEventArgs e)
        {
            await SendSaveCommandAsync();
        }

        public async Task SendSaveCommandAsync()
        {
            try
            {
                using (var pipeClient = new NamedPipeClientStream(".", "FernPipe", PipeDirection.Out))
                {
                    await pipeClient.ConnectAsync(1000);

                    using (var writer = new StreamWriter(pipeClient))
                    {
                        writer.AutoFlush = true;
                        await writer.WriteAsync("SAVE");
                    }
                }

                UpdateDaemonStatus(true);
            }
            catch (Exception)
            {
                UpdateDaemonStatus(false);
            }
        }

        private async void DaemonStatusTimer_Tick(object? sender, object e)
        {
            await RefreshDaemonStatusAsync();
        }

        private async Task RefreshDaemonStatusAsync()
        {
            if (_isCheckingDaemonStatus) return;

            _isCheckingDaemonStatus = true;
            try
            {
                bool isActive = await IsDaemonAvailableAsync();
                UpdateDaemonStatus(isActive);
            }
            finally
            {
                _isCheckingDaemonStatus = false;
            }
        }

        private async Task EnsureDaemonStartedAsync()
        {
            if (await IsDaemonAvailableAsync())
            {
                UpdateDaemonStatus(true);
                return;
            }

            if (TryStartDaemon())
            {
                bool isActive = await WaitForDaemonStatusAsync(expectedActive: true, timeout: TimeSpan.FromSeconds(4));
                UpdateDaemonStatus(isActive);
                return;
            }

            UpdateDaemonStatus(false);
        }

        private static async Task<bool> IsDaemonAvailableAsync()
        {
            try
            {
                using var pipeClient = new NamedPipeClientStream(".", "FernPipe", PipeDirection.Out, PipeOptions.Asynchronous);
                await pipeClient.ConnectAsync(200);
                return pipeClient.IsConnected;
            }
            catch (Exception)
            {
                return false;
            }
        }

        private static async Task SendDaemonCommandAsync(string command, int timeoutMilliseconds = 1000)
        {
            using var pipeClient = new NamedPipeClientStream(".", "FernPipe", PipeDirection.Out, PipeOptions.Asynchronous);
            await pipeClient.ConnectAsync(timeoutMilliseconds);

            using var writer = new StreamWriter(pipeClient);
            writer.AutoFlush = true;
            await writer.WriteAsync(command);
        }

        private static async Task<bool> WaitForDaemonStatusAsync(bool expectedActive, TimeSpan timeout)
        {
            DateTime deadline = DateTime.UtcNow + timeout;

            while (DateTime.UtcNow < deadline)
            {
                bool isActive = await IsDaemonAvailableAsync();
                if (isActive == expectedActive) return isActive;
                await Task.Delay(200);
            }

            return await IsDaemonAvailableAsync();
        }

        private async void DaemonStatusItem_Tapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
            await ToggleDaemonAsync();
        }

        private async Task ToggleDaemonAsync()
        {
            if (_isTogglingDaemon) return;

            _isTogglingDaemon = true;
            try
            {
                bool isActive = await IsDaemonAvailableAsync();
                if (isActive)
                {
                    await StopDaemonAsync();
                }
                else
                {
                    await EnsureDaemonStartedAsync();
                }
            }
            finally
            {
                _isTogglingDaemon = false;
            }
        }

        private static bool TryStartDaemon()
        {
            string? daemonPath = ResolveDaemonPath();
            if (string.IsNullOrWhiteSpace(daemonPath) || !File.Exists(daemonPath))
            {
                return false;
            }

            try
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = daemonPath,
                    WorkingDirectory = Path.GetDirectoryName(daemonPath) ?? AppContext.BaseDirectory,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });
                return true;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Fern daemon start failed: {ex.Message}");
                return false;
            }
        }

        private async Task StopDaemonAsync()
        {
            try
            {
                await SendDaemonCommandAsync("STOP");
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Fern daemon STOP command failed: {ex.Message}");
            }

            bool isActive = await WaitForDaemonStatusAsync(expectedActive: false, timeout: TimeSpan.FromSeconds(3));
            if (isActive)
            {
                TryKillKnownDaemonProcess();
                isActive = await WaitForDaemonStatusAsync(expectedActive: false, timeout: TimeSpan.FromSeconds(2));
            }

            UpdateDaemonStatus(isActive);
        }

        private static void TryKillKnownDaemonProcess()
        {
            string? daemonPath = ResolveDaemonPath();
            if (string.IsNullOrWhiteSpace(daemonPath)) return;

            string fullDaemonPath = Path.GetFullPath(daemonPath);

            foreach (Process process in Process.GetProcessesByName("Fern"))
            {
                using (process)
                {
                    try
                    {
                        string? modulePath = process.MainModule?.FileName;
                        if (modulePath == null) continue;

                        if (string.Equals(Path.GetFullPath(modulePath), fullDaemonPath, StringComparison.OrdinalIgnoreCase))
                        {
                            process.Kill(entireProcessTree: true);
                        }
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"Fern daemon kill skipped: {ex.Message}");
                    }
                }
            }
        }

        private static string? ResolveDaemonPath()
        {
            string? explicitPath = Environment.GetEnvironmentVariable("FERN_DAEMON_PATH");
            if (!string.IsNullOrWhiteSpace(explicitPath) && File.Exists(explicitPath))
            {
                return Path.GetFullPath(explicitPath);
            }

            string baseDirectory = AppContext.BaseDirectory;
            string runtime = RuntimeInformation.ProcessArchitecture switch
            {
                Architecture.X86 => "win-x86",
                Architecture.Arm64 => "win-arm64",
                _ => "win-x64"
            };

            string[] directCandidates =
            {
                Path.Combine(baseDirectory, "..", "daemon", "Fern.exe"),
                Path.Combine(baseDirectory, "daemon", "Fern.exe"),
                Path.Combine(baseDirectory, "Fern.exe")
            };

            foreach (string candidate in directCandidates)
            {
                string fullPath = Path.GetFullPath(candidate);
                if (File.Exists(fullPath)) return fullPath;
            }

            DirectoryInfo? current = new DirectoryInfo(baseDirectory);
            for (int depth = 0; current != null && depth < 10; depth++, current = current.Parent)
            {
                string[] repoCandidates =
                {
                    Path.Combine(current.FullName, "daemon", "Fern.exe"),
                    Path.Combine(current.FullName, "build", "Release", "Fern.exe"),
                    Path.Combine(current.FullName, "build", "Debug", "Fern.exe"),
                    Path.Combine(current.FullName, "ship", "work", $"Fern-{runtime}", "payload", "daemon", "Fern.exe")
                };

                foreach (string candidate in repoCandidates)
                {
                    if (File.Exists(candidate)) return Path.GetFullPath(candidate);
                }
            }

            return null;
        }

        private void UpdateDaemonStatus(bool isActive)
        {
            if (_lastDaemonActive == isActive) return;

            _lastDaemonActive = isActive;
            string label = isActive ? "Daemon actif" : "Daemon inactif";
            string tooltip = isActive ? "Cliquer pour eteindre Fern" : "Cliquer pour allumer Fern";
            DaemonStatusItem.Content = label;
            ToolTipService.SetToolTip(DaemonStatusItem, tooltip);
            DaemonStatusIcon.Foreground = new SolidColorBrush(isActive
                ? Windows.UI.Color.FromArgb(255, 62, 203, 127)
                : Windows.UI.Color.FromArgb(255, 229, 72, 77));
        }

        private void FullScreenButton_Click(object sender, RoutedEventArgs e)
        {
            ToggleFullScreen();
        }

        private void ToggleFullScreen()
        {
            if (_appWindow.Presenter.Kind == Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen)
            {
                _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
            }
            else
            {
                _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen);
            }
        }

        public void SetContentFullScreen(bool isFullScreen)
        {
            if (_isContentFullScreen == isFullScreen) return;

            _isContentFullScreen = isFullScreen;

            if (isFullScreen)
            {
                _wasWindowFullScreenBeforeContentFullScreen = _appWindow.Presenter.Kind == Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen;
                if (!_wasWindowFullScreenBeforeContentFullScreen)
                {
                    _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen);
                }

                AppChromeBar.Visibility = Visibility.Collapsed;
                NavView.Visibility = Visibility.Collapsed;
                Grid.SetRow(ContentFrame, 0);
                Grid.SetColumn(ContentFrame, 0);
                Grid.SetRowSpan(ContentFrame, 2);
                Grid.SetColumnSpan(ContentFrame, 2);
                Canvas.SetZIndex(ContentFrame, 10);
            }
            else
            {
                if (!_wasWindowFullScreenBeforeContentFullScreen)
                {
                    _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
                }

                AppChromeBar.Visibility = Visibility.Visible;
                NavView.Visibility = Visibility.Visible;
                Grid.SetRow(ContentFrame, 1);
                Grid.SetColumn(ContentFrame, 1);
                Grid.SetRowSpan(ContentFrame, 1);
                Grid.SetColumnSpan(ContentFrame, 1);
                Canvas.SetZIndex(ContentFrame, 0);
            }
        }

        private void KeyboardAccelerator_Invoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (sender.Key == Windows.System.VirtualKey.F11)
            {
                ToggleFullScreen();
                args.Handled = true;
            }
        }

        private void NavView_Loaded(object sender, RoutedEventArgs e)
        {
            NavView.SelectedItem = NavView.MenuItems[0];
            NavigateTo(typeof(Views.GalleryPage), "Ma Galerie");
        }

        private void NavView_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
        {
            if (args.IsSettingsInvoked)
            {
                NavigateTo(typeof(Views.SettingsPage), "Paramètres");
            }
            else if (args.InvokedItemContainer != null)
            {
                var tag = args.InvokedItemContainer.Tag as string;
                switch (tag)
                {
                    case "GalleryPage":
                        NavigateTo(typeof(Views.GalleryPage), "Ma Galerie");
                        break;
                    case "CloudSharesPage":
                        NavigateTo(typeof(Views.CloudSharesPage), "Partages Cloud");
                        break;
                }
            }
        }

        private void NavigateTo(Type pageType, string title)
        {
            PageTitleText.Text = title;
            ContentFrame.Navigate(pageType);
        }
    }
}
