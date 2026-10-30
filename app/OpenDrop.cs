// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Threading;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Storage.Streams;
using Forms = System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("AirDrop for Windows")]
[assembly: System.Reflection.AssemblyProduct("AirDrop for Windows")]

public sealed class NearbyDevice {
    public string Id { get; set; }
    public string Name { get; set; }
    public string Label { get; set; }
}

public sealed class OpenDropApp {
    Window window;
    Process backend;
    readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 1024 * 1024 };
    readonly object writeLock = new object();
    readonly string baseDir = AppDomain.CurrentDomain.BaseDirectory;
    readonly string dataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenDrop");
    Forms.NotifyIcon tray;
    DispatcherTimer timer;
    BluetoothLEAdvertisementPublisher publisher;
    string state = "offline", offerId = "", transferId = "", output = "", session = "";
    string[] files = new string[0];
    DateTime offerDeadline;
    bool quitting, backendEnded, trayNotice, sendBusy;
    int devicePage, historyPage;
    List<NearbyDevice> nearby = new List<NearbyDevice>();
    int ticks;
    List<Dictionary<string, object>> history = new List<Dictionary<string, object>>();

    T Get<T>(string name) where T : class { return window.FindName(name) as T; }
    void Text(string name, string value) { Get<TextBlock>(name).Text = value; }
    string Val(Dictionary<string, object> d, string key, string fallback = "") {
        object value; return d.TryGetValue(key, out value) && value != null ? Convert.ToString(value) : fallback;
    }
    object[] ArrayVal(Dictionary<string, object> d, string key) {
        object value; if (!d.TryGetValue(key, out value)) return new object[0];
        return value as object[] ?? (value is ArrayList ? ((ArrayList)value).ToArray() : new object[0]);
    }
    double Num(Dictionary<string, object> d, string key) {
        object value; return d.TryGetValue(key, out value) ? Convert.ToDouble(value) : 0;
    }
    static string Quote(string value) {
        // All arguments end in file names, not trailing directory separators.
        if (value.IndexOf('"') >= 0) throw new ArgumentException("Unexpected quote in application path");
        return "\"" + value + "\"";
    }
    void Log(string message) {
        try { Directory.CreateDirectory(dataDir); File.AppendAllText(Path.Combine(dataDir, "desktop.log"), DateTime.Now.ToString("o") + " " + message + Environment.NewLine); } catch { }
    }

    public void Start() {
        using (var stream = File.OpenRead(Path.Combine(baseDir, "MainWindow.xaml")))
            window = (Window)XamlReader.Load(stream);
        window.Icon = System.Windows.Interop.Imaging.CreateBitmapSourceFromHIcon(
            System.Drawing.Icon.ExtractAssociatedIcon(System.Reflection.Assembly.GetExecutingAssembly().Location).Handle,
            Int32Rect.Empty, System.Windows.Media.Imaging.BitmapSizeOptions.FromEmptyOptions());
        Bind();
        window.PreviewMouseWheel += (sender, e) => { e.Handled = true; };
        tray = new Forms.NotifyIcon { Icon = System.Drawing.Icon.ExtractAssociatedIcon(
            System.Reflection.Assembly.GetExecutingAssembly().Location), Text = "AirDrop for Windows", Visible = true };
        tray.DoubleClick += delegate { window.Dispatcher.BeginInvoke((Action)Show); };
        tray.ContextMenuStrip = new Forms.ContextMenuStrip();
        tray.ContextMenuStrip.Items.Add("Open AirDrop for Windows", null, delegate { window.Dispatcher.BeginInvoke((Action)Show); });
        tray.ContextMenuStrip.Items.Add("Turn off and quit", null, delegate { window.Dispatcher.BeginInvoke((Action)(() => window.Close())); });
        window.StateChanged += delegate {
            if (window.WindowState == WindowState.Minimized) {
                window.Hide();
                if (!trayNotice) { tray.ShowBalloonTip(2500, "AirDrop for Windows", "Still running here. Open the tray icon to return.", Forms.ToolTipIcon.Info); trayNotice = true; }
            }
        };
        window.Closing += (sender, e) => {
            if (!backendEnded && backend != null) {
                e.Cancel = true;
                if (!quitting) {
                    quitting = true;
                    StopBluetooth();
                    Send("shutdown");
                    SetState("stopping", "Restoring Wi-Fi before closing…");
                }
            }
        };
        window.Closed += delegate {
            if (timer != null) timer.Stop();
            StopBluetooth(); tray.Dispose();
        };
        Microsoft.Win32.SystemEvents.PowerModeChanged += (sender, e) => {
            if (e.Mode == Microsoft.Win32.PowerModes.Suspend)
                window.Dispatcher.BeginInvoke((Action)(() => Send("disable")));
        };
        timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        timer.Tick += delegate {
            if (!quitting && ++ticks % 2 == 0) Send("ping");
            if (offerId.Length > 0) {
                int remaining = Math.Max(0, (int)(offerDeadline - DateTime.UtcNow).TotalSeconds);
                Text("OfferCountdown", "Declines automatically in " + remaining + " seconds");
            }
        };
        timer.Start();
        try { StartBackend(); }
        catch (Exception ex) { backendEnded = true; Error(ex.Message); }
        new Application().Run(window);
    }

    void Bind() {
        Get<Button>("NavAirDrop").Click += delegate { Page("AirDrop"); };
        Get<Button>("DevicesPrevious").Click += delegate { devicePage = Math.Max(0, devicePage - 1); RenderDevices(); };
        Get<Button>("DevicesNext").Click += delegate { devicePage++; RenderDevices(); };
        Get<Button>("HistoryPrevious").Click += delegate { historyPage = Math.Max(0, historyPage - 1); RenderHistory(); };
        Get<Button>("HistoryNext").Click += delegate { historyPage++; RenderHistory(); };
        Get<Button>("NavHistory").Click += delegate { Page("History"); };
        Get<Button>("NavSettings").Click += delegate { Page("Settings"); };
        Get<Button>("ToggleRadio").Click += delegate {
            if (state == "online" || state == "starting") Send("disable");
            else if (state == "recovery") Send("recover");
            else Send("enable");
        };
        Get<Button>("ChooseFiles").Click += delegate {
            var picker = new Microsoft.Win32.OpenFileDialog { Multiselect = true, Title = "Choose files to AirDrop", CheckFileExists = true };
            if (picker.ShowDialog(window) == true) SelectFiles(picker.FileNames);
        };
        Get<Border>("DropZone").DragOver += (sender, e) => {
            e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; e.Handled = true;
        };
        Get<Border>("DropZone").Drop += (sender, e) => {
            if (e.Data.GetDataPresent(DataFormats.FileDrop)) SelectFiles((string[])e.Data.GetData(DataFormats.FileDrop));
        };
        Get<ListBox>("Devices").SelectionChanged += delegate { UpdateSend(); };
        Get<Button>("SendFiles").Click += delegate {
            var device = Get<ListBox>("Devices").SelectedItem as NearbyDevice;
            if (device != null) Send(new Dictionary<string, object> { {"command", "send"}, {"device", device.Id}, {"files", files} });
        };
        Get<Button>("CancelTransfer").Click += delegate { Send(new Dictionary<string, object> { {"command", "cancel"}, {"id", transferId} }); };
        Get<Button>("AcceptOffer").Click += delegate { Answer(true); };
        Get<Button>("DeclineOffer").Click += delegate { Answer(false); };
        Get<Button>("OpenReceiveFolder").Click += delegate { OpenFolder(output); };
        Get<Button>("OpenDiagnostics").Click += delegate { OpenFolder(session.Length > 0 ? session : dataDir); };
        Get<Button>("RestoreRadio").Click += delegate { Send("recover"); };
        Get<Button>("BrowseFolder").Click += delegate {
            using (var picker = new Forms.FolderBrowserDialog { Description = "Save incoming AirDrops to", SelectedPath = output })
                if (picker.ShowDialog() == Forms.DialogResult.OK) Get<TextBox>("ReceiveFolder").Text = picker.SelectedPath;
        };
        Get<Button>("SaveSettings").Click += delegate {
            var settings = new Dictionary<string, object> {
                {"name", Get<TextBox>("DeviceName").Text}, {"output", Get<TextBox>("ReceiveFolder").Text},
                {"ask_before_receiving", Get<CheckBox>("AskBeforeReceiving").IsChecked == true},
                {"limit_gib", new int[] { 1,4,8 }[Math.Max(0, Get<ComboBox>("SizeLimit").SelectedIndex)]}
            };
            Send(new Dictionary<string, object> { {"command", "settings"}, {"settings", settings} });
        };
    }

    void Page(string page) {
        foreach (string p in new[] { "AirDrop", "History", "Settings" })
            Get<StackPanel>(p + "Page").Visibility = p == page ? Visibility.Visible : Visibility.Collapsed;
        Get<Button>("NavAirDrop").Visibility = page == "AirDrop" ? Visibility.Collapsed : Visibility.Visible;
        Get<Button>("NavHistory").Visibility = page == "History" ? Visibility.Collapsed : Visibility.Visible;
        Get<Button>("NavSettings").Visibility = page == "Settings" ? Visibility.Collapsed : Visibility.Visible;
    }

    void RenderDevices() {
        int pages = Math.Max(1, (nearby.Count + 2) / 3);
        devicePage = Math.Min(devicePage, pages - 1);
        var box = Get<ListBox>("Devices");
        string selected = box.SelectedItem is NearbyDevice ? ((NearbyDevice)box.SelectedItem).Id : "";
        var visible = nearby.Skip(devicePage * 3).Take(3).ToList();
        box.ItemsSource = visible;
        box.SelectedItem = visible.FirstOrDefault(d => d.Id == selected) ?? (visible.Count == 1 ? visible[0] : null);
        box.Visibility = nearby.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        Get<StackPanel>("EmptyScene").Visibility = nearby.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        Get<StackPanel>("DevicePaging").Visibility = pages > 1 ? Visibility.Visible : Visibility.Collapsed;
        Text("DevicePageNumber", (devicePage + 1) + " / " + pages);
        Get<Button>("DevicesPrevious").IsEnabled = devicePage > 0;
        Get<Button>("DevicesNext").IsEnabled = devicePage + 1 < pages;
        UpdateSend();
    }

    void StartBackend() {
        var config = json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(baseDir, "app-config.json")));
        var executable = Path.GetFullPath(Path.Combine(baseDir, Val(config, "backend")));
        var args = ArrayVal(config, "backend_args").Select(x => Quote(Convert.ToString(x))).ToList();
        args.Add("--config"); args.Add(Quote(Path.Combine(baseDir, "app-config.json")));
        args.Add("--data"); args.Add(Quote(dataDir));
        backend = new Process {
            StartInfo = new ProcessStartInfo(executable, String.Join(" ", args)) {
                WorkingDirectory = baseDir, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            }, EnableRaisingEvents = true
        };
        backend.OutputDataReceived += (sender, e) => {
            if (e.Data == null) return;
            try {
                var message = new JavaScriptSerializer { MaxJsonLength = 1024 * 1024 }.Deserialize<Dictionary<string, object>>(e.Data);
                window.Dispatcher.BeginInvoke((Action)(() => {
                    try { Event(message); } catch (Exception ex) { Log("UI event: " + ex); Error("An app update could not be displayed. See diagnostics."); }
                }));
            } catch (Exception ex) { Log("Backend output: " + ex.Message); }
        };
        backend.ErrorDataReceived += (sender, e) => { if (e.Data != null) Log(e.Data); };
        backend.Exited += delegate {
            window.Dispatcher.BeginInvoke((Action)(() => {
                backendEnded = true;
                if (quitting) window.Close();
                else { StopBluetooth(); SetState("error", "The app service stopped. Close and reopen AirDrop for Windows."); }
            }));
        };
        backend.Start();
        backend.StandardInput.AutoFlush = true;
        backend.BeginOutputReadLine(); backend.BeginErrorReadLine();
    }

    void Send(string command) { Send(new Dictionary<string, object> { {"command", command} }); }
    void Send(Dictionary<string, object> value) {
        try {
            lock (writeLock) {
                if (backend != null && !backend.HasExited) {
                    // Raw UTF-8 avoids .NET's current console code page.
                    byte[] bytes = Encoding.UTF8.GetBytes(json.Serialize(value) + "\n");
                    backend.StandardInput.BaseStream.Write(bytes, 0, bytes.Length);
                    backend.StandardInput.BaseStream.Flush();
                }
            }
        } catch (Exception ex) { Log("Send command: " + ex.Message); }
    }

    void Event(Dictionary<string, object> value) {
        string kind = Val(value, "event");
        if (kind == "hello") {
            Settings((Dictionary<string, object>)value["settings"]);
            history = ArrayVal(value, "history").Cast<Dictionary<string, object>>().ToList();
            RenderHistory();
            session = Val(value, "recovery");
            SetState(Val(value, "status"), "");
        } else if (kind == "state") SetState(Val(value, "status"), Val(value, "detail"));
        else if (kind == "settings") { Settings((Dictionary<string, object>)value["settings"]); Text("Footer", "Preferences saved."); }
        else if (kind == "session") session = Val(value, "path");
        else if (kind == "error") Error(Val(value, "message"));
        else if (kind == "devices") {
            nearby = ArrayVal(value, "devices").Cast<Dictionary<string, object>>().Select(d => new NearbyDevice {
                Id = Val(d, "id"), Name = Val(d, "name"), Label = Val(d, "name")
            }).ToList();
            RenderDevices();
        } else if (kind == "auto_accepted") {
            Text("Footer", "Receiving automatically from " + Val(value, "sender"));
        } else if (kind == "offer") {
            offerId = Val(value, "id"); offerDeadline = DateTime.UtcNow.AddSeconds(Num(value, "seconds"));
            Text("OfferSender", Val(value, "sender") + " wants to share");
            Text("OfferFiles", String.Join("\n", ArrayVal(value, "files").Take(6).Select(Convert.ToString)));
            Get<Border>("OfferOverlay").Visibility = Visibility.Visible;
            Show();
            tray.ShowBalloonTip(4000, "Incoming AirDrop", Val(value, "sender") + " wants to share files.", Forms.ToolTipIcon.Info);
            Get<Button>("DeclineOffer").Focus();
        } else if (kind == "offer_closed") {
            if (Val(value, "id") == offerId) { offerId = ""; Get<Border>("OfferOverlay").Visibility = Visibility.Collapsed; }
        } else if (kind == "receiving" || kind == "sending") {
            transferId = Val(value, "id");
            Get<Border>("TransferCard").Visibility = Visibility.Visible;
            Text("TransferTitle", (kind == "receiving" ? "Receiving " : "Sending ") + ArrayVal(value, "files").Length + " file(s)");
            Text("TransferDetail", "Preparing transfer…");
            Get<ProgressBar>("TransferProgress").IsIndeterminate = true;
            Get<Button>("CancelTransfer").Visibility = Visibility.Visible;
            if (kind == "sending") { sendBusy = true; UpdateSend(); }
        } else if (kind == "receive_progress" || kind == "send_progress") {
            if (Val(value, "id") != transferId) return;
            double done = Num(value, "done"), total = Num(value, "total");
            var bar = Get<ProgressBar>("TransferProgress");
            bar.IsIndeterminate = total <= 0;
            bar.Value = total > 0 ? 100 * done / total : 0;
            string phase = Val(value, "phase", "receiving");
            Text("TransferDetail", char.ToUpper(phase[0]) + phase.Substring(1) + " · " + Bytes(done) + (total > 0 ? " of " + Bytes(total) : ""));
        } else if (kind == "received" || kind == "sent" || kind == "receive_failed" || kind == "send_failed") {
            bool ok = kind == "received" || kind == "sent";
            Get<ProgressBar>("TransferProgress").IsIndeterminate = false;
            Get<ProgressBar>("TransferProgress").Value = ok ? 100 : 0;
            Text("TransferTitle", ok ? (kind == "sent" ? "Sent successfully" : "Saved to your receive folder") : "Transfer ended");
            Text("TransferDetail", ok ? "View it in Recent." : Val(value, "error"));
            Get<Button>("CancelTransfer").Visibility = Visibility.Collapsed;
            if (kind == "sent" || kind == "send_failed") sendBusy = false;
            UpdateSend();
            if (ok && kind == "received") tray.ShowBalloonTip(3000, "AirDrop received", "Your files have been saved.", Forms.ToolTipIcon.Info);
        } else if (kind == "history") {
            history.Add((Dictionary<string, object>)value["item"]); historyPage = 0; RenderHistory();
        }
    }

    void Settings(Dictionary<string, object> settings) {
        output = Val(settings, "output");
        Get<TextBox>("DeviceName").Text = Val(settings, "name");
        Get<TextBox>("ReceiveFolder").Text = output;
        Text("SidebarName", Val(settings, "name"));
        Get<CheckBox>("AskBeforeReceiving").IsChecked = settings.ContainsKey("ask_before_receiving") && Convert.ToBoolean(settings["ask_before_receiving"]);
        Get<ComboBox>("SizeLimit").SelectedIndex = Num(settings, "limit_gib") == 8 ? 2 : Num(settings, "limit_gib") == 1 ? 0 : 1;
    }
    void SetState(string next, string detail) {
        state = next;
        bool online = state == "online", busy = state == "stopping" || state == "restoring";
        string label = online ? "Available" : state == "starting" ? "Starting…" : busy ? "Restoring Wi-Fi…" : state == "recovery" ? "Needs recovery" : state == "error" ? "Unavailable" : "Offline";
        Text("StatusBadge", label);
        Get<System.Windows.Shapes.Ellipse>("StatusDot").Fill = new SolidColorBrush(online ? Color.FromRgb(48,183,114) : Color.FromRgb(162,166,174));
        Text("HeroTitle", online ? "Ready to share" : state == "starting" ? "Getting ready…" : busy ? "Restoring your Wi-Fi" : state == "recovery" ? "Let's restore Wi-Fi" : state == "error" ? "AirDrop needs attention" : "Ready to share");
        Text("HeroDescription", online ? "On your iPhone, choose Everyone for 10 Minutes." : detail.Length > 0 ? detail : "Turn on to find nearby devices.");
        var button = Get<Button>("ToggleRadio");
        button.Content = online ? "Turn off" : state == "starting" ? "Cancel" : busy ? "Please wait…" : state == "recovery" ? "Restore Wi-Fi" : "Turn on";
        button.IsEnabled = !busy && !quitting && !backendEnded;
        bool editable = state == "offline" || state == "error";
        Get<Button>("SaveSettings").IsEnabled = editable || online;
        Get<TextBox>("DeviceName").IsEnabled = editable;
        Get<TextBox>("ReceiveFolder").IsEnabled = editable;
        Get<Button>("BrowseFolder").IsEnabled = editable;
        Get<ComboBox>("SizeLimit").IsEnabled = editable;
        Get<CheckBox>("AskBeforeReceiving").IsEnabled = editable || online;
        Get<Button>("RestoreRadio").Visibility = state == "recovery" ? Visibility.Visible : Visibility.Collapsed;
        Text("Footer", online ? (Get<CheckBox>("AskBeforeReceiving").IsChecked == true ? "Ask before receiving" : "Receive from everyone") : detail.Length > 0 ? detail : "Receive from everyone");
        if (online) StartBluetooth(); else StopBluetooth();
        UpdateSend();
    }
    void Error(string message) { Text("Footer", message); Get<TextBlock>("Footer").ToolTip = message; Log(message); }
    void UpdateSend() { Get<Button>("SendFiles").IsEnabled = state == "online" && !sendBusy && files.Length > 0 && Get<ListBox>("Devices").SelectedItem != null; }
    void SelectFiles(string[] values) {
        if (values.Length > 256 || values.Any(p => !File.Exists(p))) { Error("Choose up to 256 files. Folders are not supported yet."); return; }
        files = values;
        Text("FileSummary", values.Length == 1 ? Path.GetFileName(values[0]) : values.Length + " files selected");
        try { Text("FileSize", Bytes(values.Sum(p => new FileInfo(p).Length)) + " · Ready to share"); }
        catch (Exception ex) { Error(ex.Message); }
        UpdateSend();
    }
    static string Bytes(double value) {
        string[] suffix = {"B", "KB", "MB", "GB"}; int i = 0;
        while (value >= 1024 && i < suffix.Length - 1) { value /= 1024; i++; }
        return value.ToString(i == 0 ? "0" : "0.0") + " " + suffix[i];
    }
    void Answer(bool accept) {
        if (offerId.Length == 0) return;
        Send(new Dictionary<string, object> { {"command", accept ? "accept" : "decline"}, {"id", offerId} });
        Get<Border>("OfferOverlay").Visibility = Visibility.Collapsed;
    }
    void RenderHistory() {
        var panel = Get<StackPanel>("HistoryItems");
        panel.Children.Clear();
        int pages = Math.Max(1, (history.Count + 4) / 5);
        historyPage = Math.Min(historyPage, pages - 1);
        Text("HistoryPageNumber", (historyPage + 1) + " / " + pages);
        Get<Button>("HistoryPrevious").IsEnabled = historyPage > 0;
        Get<Button>("HistoryNext").IsEnabled = historyPage + 1 < pages;
        Get<StackPanel>("HistoryPaging").Visibility = pages > 1 ? Visibility.Visible : Visibility.Hidden;
        if (history.Count == 0) {
            panel.Children.Add(new TextBlock { Text = "Nothing shared yet.", Foreground = Brushes.Gray, Margin = new Thickness(0,20,0,0) });
            return;
        }
        foreach (var item in history.AsEnumerable().Reverse().Skip(historyPage * 5).Take(5)) {
            var row = new DockPanel { Height = 60 };
            string path = Val(item, "path");
            if (path.Length > 0) {
                var button = new Button { Content = "Show", Padding = new Thickness(12,7,12,7), Margin = new Thickness(14,0,0,0), VerticalAlignment = VerticalAlignment.Center };
                button.Click += delegate { OpenFolder(path); }; DockPanel.SetDock(button, Dock.Right); row.Children.Add(button);
            }
            var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            var names = ArrayVal(item, "files").Select(Convert.ToString).ToArray();
            text.Children.Add(new TextBlock { Text = names.Length == 1 ? Path.GetFileName(names[0]) : names.Length + " files", FontWeight = FontWeights.Medium, TextTrimming = TextTrimming.CharacterEllipsis });
            string status = Val(item, "status") == "complete" ? Val(item, "direction") : Val(item, "error");
            text.Children.Add(new TextBlock { Text = status + " · " + Val(item, "time").Replace("T", "  "), FontSize = 11, Foreground = Brushes.Gray, Margin = new Thickness(0,4,0,0), TextTrimming = TextTrimming.CharacterEllipsis });
            row.Children.Add(text); panel.Children.Add(row);
        }
    }
    void OpenFolder(string path) {
        try {
            if (!Directory.Exists(path)) { Error("This folder is not available yet."); return; }
            Process.Start(new ProcessStartInfo("explorer.exe", Quote(path)) { UseShellExecute = true });
        } catch (Exception ex) { Error(ex.Message); }
    }
    void Show() { window.Show(); window.WindowState = WindowState.Normal; window.Activate(); }

    void StartBluetooth() {
        if (publisher != null) return;
        try {
            // AirDrop sender wakeup only. No Swift Pair or device-pairing advertisement.
            byte[] bytes = new byte[] { 0x05,0x12,0x40,0,0,0,0,0,0,0,0x03,0,0,0,0,0,0,0,0,0 };
            byte[] random = new byte[3];
            using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(random);
            System.Array.Copy(random, 0, bytes, 3, 3);
            publisher = new BluetoothLEAdvertisementPublisher();
            using (var writer = new DataWriter()) {
                writer.WriteBytes(bytes);
                publisher.Advertisement.ManufacturerData.Add(new BluetoothLEManufacturerData(0x004c, writer.DetachBuffer()));
            }
            publisher.StatusChanged += (sender, args) => {
                if (args.Status == BluetoothLEAdvertisementPublisherStatus.Aborted)
                    window.Dispatcher.BeginInvoke((Action)(() => { Log("Bluetooth wakeup: " + args.Error); Text("Footer", "Keep AirDrop settings open on your iPhone while finding devices."); }));
            };
            publisher.Start();
        } catch (Exception ex) { Log("Bluetooth: " + ex); StopBluetooth(); }
    }
    void StopBluetooth() {
        if (publisher == null) return;
        try { publisher.Stop(); } catch (Exception ex) { Log(ex.Message); }
        publisher = null;
    }

    [STAThread]
    public static void Main() {
        bool created;
        using (var mutex = new Mutex(true, "Local\\OpenDropDesktop", out created)) {
            if (!created) { MessageBox.Show("AirDrop for Windows is already running. Open it from the system tray.", "AirDrop for Windows"); return; }
            try { new OpenDropApp().Start(); }
            catch (Exception ex) {
                string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenDrop");
                Directory.CreateDirectory(dir);
                File.AppendAllText(Path.Combine(dir, "desktop.log"), ex.ToString());
                MessageBox.Show("AirDrop for Windows could not start: " + ex.Message, "AirDrop for Windows");
            }
        }
    }
}
