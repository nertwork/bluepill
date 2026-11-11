class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Web UI for BluePill"
  homepage "https://github.com/nertwork/bluepill"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.1.0.tar.gz"
  sha256 "013462dd73009ee72d54379f23fb6f6befd51c9be09bc42d78be5db26d9311c0"
  version "1.1.0"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"


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
