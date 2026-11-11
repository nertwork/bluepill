class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Web UI for BluePill"
  homepage "https://github.com/nertwork/bluepill"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.1.0.tar.gz"
  sha256 "443821dea7e723e79f31ecd903e0f50828640c922dbc991d4c25f99c6bf42d66"
  version "1.1.0"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"

  bottle do
    root_url "https://github.com/nertwork/bluepill/releases/download/v1.1.0"
    sha256 cellar: :any_skip_relocation, all: "443821dea7e723e79f31ecd903e0f50828640c922dbc991d4c25f99c6bf42d66"
  end

  def install
    bin.install "server/bluepill_server.py" => "bluepill-server"
    bin.install_symlink "bluepill-server" => "imsgd"
  end

  service do
    run [opt_bin/"bluepill-server", "--port", "1235"]
    keep_alive true
    working_dir var
    log_path var/"log/bluepill-server.log"
    error_log_path var/"log/bluepill-server.err.log"
  end

  test do
    assert_match "bluepill-server 1.1.0", shell_output("#{bin}/bluepill-server --version")
  end
end
