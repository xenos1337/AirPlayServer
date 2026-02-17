using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using IWshRuntimeLibrary;

namespace AirPlayInstaller
{
    public partial class Main : Form
    {
        public Main()
        {
            InitializeComponent();
        }

        private void Form1_Load(object sender, EventArgs e)
        {

        }

        private async void Install_Click(object sender, EventArgs e)
        {
            status.Text = "Installing..";
            Install.Enabled = false;
            progressBar1.Value = 0;

            try
            {
                string cdn = string.Empty;

                using (WebClient wc = new WebClient())
                {
                    status.Text = "Fetching download URL..";
                    cdn = await wc.DownloadStringTaskAsync("https://raw.githubusercontent.com/cledtz/AirPlayServer/refs/heads/master/latest_release.md");

                    if (string.IsNullOrEmpty(cdn))
                    {
                        MessageBox.Show("Could not get cdn.");
                        Install.Enabled = true;
                        status.Text = "Waiting..";
                        return;
                    }

                    cdn = cdn.Trim();
                    progressBar1.Value = 10;

                    string installDir = @"C:\Program Files\AirPlay";
                    string tempZip = Path.Combine(Path.GetTempPath(), "AirPlay.zip");

                    // Download the zip file
                    status.Text = "Downloading..";
                    wc.DownloadProgressChanged += (s, ev) =>
                    {
                        // Map download progress to 10-70 range
                        progressBar1.Value = 10 + (int)(ev.ProgressPercentage * 0.6);
                    };
                    await wc.DownloadFileTaskAsync(new Uri(cdn), tempZip);
                    progressBar1.Value = 70;

                    // Create install directory
                    status.Text = "Extracting..";
                    if (Directory.Exists(installDir))
                        Directory.Delete(installDir, true);
                    Directory.CreateDirectory(installDir);

                    // Extract zip
                    ZipFile.ExtractToDirectory(tempZip, installDir);
                    progressBar1.Value = 85;

                    // Clean up temp zip
                    System.IO.File.Delete(tempZip);

                    // Find the main executable
                    string[] exeFiles = Directory.GetFiles(installDir, "*.exe", SearchOption.AllDirectories);
                    string exePath = exeFiles.Length > 0 ? exeFiles[0] : installDir;

                    // Create Desktop shortcut
                    status.Text = "Creating shortcuts..";
                    string desktopPath = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                    CreateShortcut(Path.Combine(desktopPath, "AirPlay.lnk"), exePath);

                    // Create Start Menu shortcut
                    string startMenuPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonStartMenu), "Programs");
                    CreateShortcut(Path.Combine(startMenuPath, "AirPlay.lnk"), exePath);

                    progressBar1.Value = 100;
                    status.Text = "Installation complete!";
                    MessageBox.Show("AirPlay has been installed successfully!", "Installation Complete", MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Installation failed: " + ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                status.Text = "Installation failed.";
                Install.Enabled = true;
            }
        }

        private void Uninstall_Click(object sender, EventArgs e)
        {
            string installDir = @"C:\Program Files\AirPlay";

            if (!Directory.Exists(installDir))
            {
                MessageBox.Show("AirPlay is not installed.", "Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            DialogResult result = MessageBox.Show("Are you sure you want to uninstall AirPlay?", "Confirm Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (result != DialogResult.Yes)
                return;

            Install.Enabled = false;
            Uninstall.Enabled = false;
            progressBar1.Value = 0;

            try
            {
                // Delete Desktop shortcut
                status.Text = "Removing shortcuts..";
                string desktopShortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "AirPlay.lnk");
                if (System.IO.File.Exists(desktopShortcut))
                    System.IO.File.Delete(desktopShortcut);
                progressBar1.Value = 25;

                // Delete Start Menu shortcut
                string startMenuShortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonStartMenu), "Programs", "AirPlay.lnk");
                if (System.IO.File.Exists(startMenuShortcut))
                    System.IO.File.Delete(startMenuShortcut);
                progressBar1.Value = 50;

                // Delete install directory
                status.Text = "Removing files..";
                Directory.Delete(installDir, true);
                progressBar1.Value = 100;

                status.Text = "Uninstall complete!";
                MessageBox.Show("AirPlay has been uninstalled successfully!", "Uninstall Complete", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Uninstall failed: " + ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                status.Text = "Uninstall failed.";
            }

            Install.Enabled = true;
            Uninstall.Enabled = true;
        }

        private void CreateShortcut(string shortcutPath, string targetPath)
        {
            WshShell shell = new WshShell();
            IWshShortcut shortcut = (IWshShortcut)shell.CreateShortcut(shortcutPath);
            shortcut.TargetPath = targetPath;
            shortcut.WorkingDirectory = Path.GetDirectoryName(targetPath);
            shortcut.Description = "AirPlay";
            shortcut.Save();
        }
    }
}
