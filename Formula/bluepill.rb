class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Web UI for BluePill"
  homepage "https://github.com/nertwork/bluepill"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.2.0.tar.gz"
  sha256 "507ddaf8dcd2e2faa68ec19aa532118bdc7c6269b1e294ac7544ea505b82ef6d"
  version "1.2.0"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"

  bottle do
    root_url "https://github.com/nertwork/bluepill/releases/download/v1.2.0"
    sha256 cellar: :any_skip_relocation, all: "5d8320fcdc108e9187856ef91b29a54ce7fa1cfce87aeac2b52546f8e4d53631"
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
    assert_match "bluepill-server 1.2.0", shell_output("#{bin}/bluepill-server --version")
    help_out = shell_output("#{bin}/bluepill-server --help")
    if help_out.include?("preflight")
      assert_match "\"status\":", shell_output("#{bin}/bluepill-server preflight --no-fix")
    else
      system bin/"bluepill-server", "--selftest"
    end
  end
end
