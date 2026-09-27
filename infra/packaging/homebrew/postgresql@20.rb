# Pinned development SDK for the experimental BioseqDB PG20 Mac installer.
# Keep this independent of the upstream-maintainer PG19 beta formula.
class PostgresqlAT20 < Formula
  desc "Pinned PostgreSQL 20 development SDK for BioseqDB alpha packages"
  homepage "https://www.postgresql.org/"
  url "https://codeload.github.com/postgres/postgres/tar.gz/df3978c2340d79c6319f8239a69abbd27b1069df"
  version "20.0devel20260924"
  sha256 "bbd9339e7628c8efad180cedf56c2adb5af6da2a4ae045f5117de1c928119f2c"
  license "PostgreSQL"

  keg_only :versioned_formula
  depends_on "bison" => :build
  depends_on "flex" => :build
  depends_on "pkgconf" => :build
  depends_on "gettext"
  depends_on "icu4c@78"
  depends_on "lz4"
  depends_on "openssl@3"
  depends_on "readline"
  depends_on "zstd"
  uses_from_macos "zlib"

  def install
    dependencies = %w[gettext icu4c@78 lz4 openssl@3 readline zstd]
    system "./configure", "--prefix=#{prefix}", "--enable-nls", "--with-icu",
           "--with-openssl", "--with-lz4", "--with-zstd",
           "--with-includes=#{dependencies.map { |name| Formula[name].opt_include }.join(":")}",
           "--with-libraries=#{dependencies.map { |name| Formula[name].opt_lib }.join(":")}",
           "--with-extra-version= (BioseqDB SDK df3978c2340d)"
    system "make"
    system "make", "install"
  end

  test do
    assert_match "PostgreSQL 20devel", shell_output("#{bin}/pg_config --version")
  end
end
