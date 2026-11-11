class Bluepill < Formula
  desc "Headless zero-Electron macOS iMessage, SMS & RCS relay daemon + Web UI for BluePill"
  homepage "https://github.com/nertwork/bluepill"
  url "https://github.com/nertwork/bluepill/archive/refs/tags/v1.1.0.tar.gz"
  sha256 "7d3f00d895aeca32342ea8f899c942b8925ab272395e163a89763fe07bde5b8d"
  version "1.1.0"
  license "MIT"
  head "https://github.com/nertwork/bluepill.git", branch: "main"

  bottle do
    root_url "https://github.com/nertwork/bluepill/releases/download/v1.1.0"
    sha256 cellar: :any_skip_relocation, all: "fde345c6cf4774251f000626ea67fed4a5a2104c2b53e46151d51f834b6e2bb7"
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
