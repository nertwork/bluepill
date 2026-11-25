class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Cloud Relay + Web UI for BluePill"
  homepage "https://bluepill.app"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.2.16.tar.gz"
  sha256 "81186034e2c48595fe07179fefab57c3740c9743821c80ebc6d5cfb1d4ea4ca1"
  version "1.2.16"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"

  bottle do
    root_url "https://github.com/nertwork/bluepill/releases/download/v1.2.16"
    sha256 cellar: :any_skip_relocation, all: "10f6cb8346b9487b8e70a0ab6b532f3b1a1cc985fac0a817451f114ec425ea60"
  end

  def install
    bin.install "server/bluepill_server.py" => "bluepill-server"
    bin.install_symlink "bluepill-server" => "imsgd"
  end

  def post_install
    help_out = Utils.safe_popen_read(bin/"bluepill-server", "--help")
    if help_out.include?("preflight")
      system bin/"bluepill-server", "preflight", "--no-fix"
    else
      system bin/"bluepill-server", "--selftest"
    end
  rescue StandardError
    nil
  end

  def caveats
    <<~EOS
      BluePill Cloud Setup (Default — Zero Port-Forwarding):
        1. bluepill-server login
           (Opens https://bluepill.app/portal to link your Mac to BluePill Cloud)
        2. brew services start bluepill
        3. Grant macOS Permissions (System Settings -> Privacy & Security):
           - Full Disk Access: allow Terminal / Homebrew Python (reads chat.db, Notes, Calendar, Photos)
           - Automation -> Messages: allow Python / bluepill-server to control Messages.app (required for sending)
        4. Verify everything is green:
           bluepill-server doctor

      Cloud Managed Updates (BluePill Cloud Perk):
        When linked to BluePill Cloud, background stable update checks run
        automatically and require explicit confirmation in the UI or via:
          bluepill-server update-status
          bluepill-server update --confirm

      Local-Only / iCloud Hub Preflight:
        Verify Full Disk Access for Messages, Notes, Calendar, and Photos:
          bluepill-server preflight
        Check Cloud Relay status at any time:
          bluepill-server relay-status

      All three iCloud Hub modules (Notes, Calendar, Photos) are enabled by default
      and configured in ~/.config/bluepill/server.json (relay config in ~/.config/bluepill/relay.json).
      Run local-only without Cloud Relay using: bluepill-server serve --no-relay
    EOS
  end

  service do
    run [opt_bin/"bluepill-server", "serve", "--port", "1235"]
    keep_alive true
    working_dir var
    log_path var/"log/bluepill-server.log"
    error_log_path var/"log/bluepill-server.err.log"
  end

  test do
    assert_match "bluepill-server 1.2.16", shell_output("#{bin}/bluepill-server --version")
    help_out = shell_output("#{bin}/bluepill-server --help")
    if help_out.include?("preflight")
      assert_match "\"status\":", shell_output("#{bin}/bluepill-server preflight --no-fix")
    else
      system bin/"bluepill-server", "--selftest"
    end
  end
end
