class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Web UI for BluePill"
  homepage "https://github.com/nertwork/bluepill"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.2.1.tar.gz"
  sha256 "bf704a27148ca5dd7c30318b8a40182ce3f4e41d6b91b5233b24c1994edbfa82"
  version "1.2.1"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"

  bottle do
    root_url "https://github.com/nertwork/bluepill/releases/download/v1.2.1"
    sha256 cellar: :any_skip_relocation, all: "e9fc20a544a42a0f2d4b07f66ed3f6164940d3696090e60ef184636911de1747"
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
    assert_match "bluepill-server 1.2.1", shell_output("#{bin}/bluepill-server --version")
    help_out = shell_output("#{bin}/bluepill-server --help")
    if help_out.include?("preflight")
      assert_match "\"status\":", shell_output("#{bin}/bluepill-server preflight --no-fix")
    else
      system bin/"bluepill-server", "--selftest"
    end
  end
end
