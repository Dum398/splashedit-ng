pkgname=splashedit-ng
pkgver=2.4.1
pkgrel=1
pkgdesc="Standalone scene editor for psxsplash, replacing the Unity-based SplashEdit."
arch=('x86_64')
url="https://github.com/psxsplash/splashedit-ng"
license=('MIT')

build() {
  cd ..
  mkdir -p build
  cmake -S . -B build -G Ninja
  cmake --build build
}

package() {
  install -Dm755 build/splashedit "$pkgdir/usr/bin/splashedit"
}
