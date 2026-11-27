# xprism-control development shell: nix-shell
{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  name = "xprism-control-dev";
  nativeBuildInputs = with pkgs; [ meson ninja pkg-config gcc ];
  buildInputs = with pkgs; [ libxcb gtk4 ];
  # the tests: XPrism's Xvfb (../xserver/build) and a few X tools
  packages = with pkgs; [ xauth xwd xwininfo xdotool ];
}
