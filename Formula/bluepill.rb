class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Web UI for BluePill"
  homepage "https://github.com/nertwork/bluepill"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.2.2.tar.gz"
  sha256 "bbe3028b4828231650f62728544c655088fb7861c2081ccdb05a262775f783fd"
  version "1.2.2"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"

  bottle do
    root_url "https://github.com/nertwork/bluepill/releases/download/v1.2.2"
    sha256 cellar: :any_skip_relocation, all: "2b8de0350a5e5e03afe75d8cc65aa917e9d032b1f6d76c0e1de7b9bd663d1889"
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
      Run the iCloud Hub & macOS preflight check (verifies Full Disk Access for Messages,
      Apple Notes, Calendar, and Photos, and auto-enables photolibraryd & cloudphotod):
        bluepill-server preflight

      All three iCloud Hub modules (Notes, Calendar, Photos) are enabled by default.
      Module configuration is saved to ~/.config/bluepill/server.json. You can toggle
      modules via CLI and apply changes to the running relay by restarting the service:
        bluepill-server --enable-notes --enable-calendar --enable-photos preflight --no-fix
        bluepill-server --disable-photos preflight --no-fix
        brew services restart bluepill
    EOS
  end

  service do
    run [opt_bin/"bluepill-server", "--port", "1235"]
    keep_alive true
    working_dir var
    log_path var/"log/bluepill-server.log"
    error_log_path var/"log/bluepill-server.err.log"
  end

  test do
    assert_match "bluepill-server 1.2.2", shell_output("#{bin}/bluepill-server --version")
    help_out = shell_output("#{bin}/bluepill-server --help")
    if help_out.include?("preflight")
      assert_match "\"status\":", shell_output("#{bin}/bluepill-server preflight --no-fix")
    else
      system bin/"bluepill-server", "--selftest"
    end
  end
end
