# ShapoGFX

[English](README.md) | 日本語

組込みシステム向けの 2D/3D グラフィックスライブラリです。

ShapoGFX は、小型ディスプレイを駆動するマイコン向けの、依存ライブラリを持たない
小さな C++17 ソフトウェアレンダラです。ビットマップフォント付きの 2D 描画 API と、
フレームバッファも Z バッファも必要としないスキャンライン方式の 3D レンダラを備えます。
ライブラリ自身はメモリを確保しません。描画先には利用者が用意したバッファを使い、
作業用メモリは利用者から渡されたアリーナから取ります。

- **ドキュメント (日本語):** https://shapoco.github.io/shapo-gfx/ref/
- **ライブデモ:** [demo2d](https://shapoco.github.io/shapo-gfx/example/demo2d/),
  [demo3d](https://shapoco.github.io/shapo-gfx/example/demo3d/),
  [demorig](https://shapoco.github.io/shapo-gfx/example/demorig/)
- **設計仕様 (英語):** [SPEC.md](SPEC.md)

## 特長

- ピクセルフォーマット GRAY1、RGB444、ARGB4444、RGB565_SWAPPED (バイトスワップ済みで、
  ディスプレイコントローラへそのまま DMA 転送可能) と、オプトインで RGB565
  (ネイティブバイトオーダー、16 ビットディスプレイインタフェース向け)。
  使わないフォーマットはコンパイル対象から外せます
- `Graphics2D`: 図形、直線、ポリゴン、アルファ/加算ブレンド付きの転送、
  2 色ビットマップ、GFXfont によるテキスト描画 (フォント 4 種同梱)
- `rig`: DragonBones キャラクタの 2D スケルタルアニメーション (ボーン、スロット、
  任意のフレームレートで補間されるキーフレーム、描画順)。描画には `Graphics2D` を使用
- `Graphics3D`: 画面の任意の領域をバンドバッファへ描画するスキャンラインラスタライザ。
  グーローシェーディング、任意フォーマットのテクスチャ、環境マッピング、
  アルファ/加算ブレンド、パースペクティブコレクトなテクスチャマッピング、組込み形状、
  アニメーション用のビジターフックを持つ静的シーングラフ
- ツール: `img2cpp` (画像)、`gltf2cpp` (glTF 2.0 モデル)、`dbones2cpp`
  (DragonBones アーマチュア) で `const` データのヘッダを生成
- ASan/UBSan 下で実行することを想定した自己検査型のテスト

## アプリケーション例

- [Devour Sphere](https://github.com/shapoco/devour-sphere): RP2350 / RP2040 /
  ESP32-S3 / ESP32-P4 搭載ボードとブラウザで動作する 3D シューティングゲーム。
  ShapoGFX のショーケースとして作成

## インストール

ShapoGFX はリポジトリを一度 clone し、環境変数 `SHAPOGFX_PATH` でその場所を指すのが
標準的な使い方です。ビルドファイルからはパスを直接書かずにこの変数で参照します。

1. `${HOME}/sgfx/` を作成し、そこに移動します:

   ```sh
   mkdir -p ${HOME}/sgfx
   cd ${HOME}/sgfx
   ```

2. リポジトリを clone します:

   ```sh
   git clone https://github.com/shapoco/shapo-gfx.git
   ```

3. 環境変数 `SHAPOGFX_PATH` に `${HOME}/sgfx/shapo-gfx` を設定します。
   新しいシェルでも有効にするには `~/.bashrc` (使っているシェルの設定ファイル) にも同じ行を追加してください:

   ```sh
   export SHAPOGFX_PATH=${HOME}/sgfx/shapo-gfx
   echo 'export SHAPOGFX_PATH=${HOME}/sgfx/shapo-gfx' >> ~/.bashrc
   ```

## ビルド

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

他の CMake プロジェクト (Pico SDK のプロジェクトを含む) から使う場合:

```cmake
add_subdirectory($ENV{SHAPOGFX_PATH} shapo-gfx)
target_link_libraries(your_target PRIVATE shapoco::gfx)
```

### ESP-IDF

リポジトリはそのまま `shapo-gfx` という名前の ESP-IDF コンポーネントになっています。
プロジェクトの `CMakeLists.txt` でコンポーネントのディレクトリに加えます。

```cmake
set(EXTRA_COMPONENT_DIRS $ENV{SHAPOGFX_PATH})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(your_project)
```

ローカルに clone せずに使う場合は `main/idf_component.yml` の依存関係に加えます。

```yaml
dependencies:
  shapo-gfx:
    git: https://github.com/shapoco/shapo-gfx.git
    version: v1.5.0
```

(`git:` と `version:` の代わりに `path: ${SHAPOGFX_PATH}` と書くと、ローカルの clone を参照します)。
キーは必ずディレクトリ名と同じ `shapo-gfx` にしてください。
どちらの場合も、使う側のコンポーネントで `REQUIRES` に指定します。

```cmake
idf_component_register(SRCS "app_main.cpp" REQUIRES shapo-gfx)
```

コンパイル時オプションは `idf.py menuconfig` の「ShapoGFX」で設定します
(`sdkconfig.defaults` には `CONFIG_SHAPOGFX_FORMAT_GRAY1=n` のように書きます)。

### PlatformIO

`library.json` があるので、このリポジトリを PlatformIO ライブラリとして使用できます:

```ini
[env:my_board]
platform = espressif32
board = seeed_xiao_esp32s3
framework = arduino
lib_deps = symlink://${sysenv.SHAPOGFX_PATH}
; ローカルに clone せず GitHub から直接取得する場合:
; lib_deps = https://github.com/shapoco/shapo-gfx.git
; the headers are C++17; many cores still default to gnu++11
build_unflags = -std=gnu++11
build_flags = -std=gnu++17
```

### ビルドシステムを使わない場合

`include/` をインクルードパスに追加し、`src/gfx2d/*.cpp` と `src/gfx3d/*.cpp` を
C++17 でコンパイルしてください。

Python 製ツールとドキュメント生成の依存パッケージ:

```sh
python3 -m pip install -r requirements.txt
```

## リポジトリ構成

```
include/shapoco/gfx2d/   2D API と共通の型 (ピクセルフォーマット, Surface, Graphics2D, フォント, rig)
include/shapoco/gfx3d/   3D レンダラ (Graphics3D, 形状, 静的シーン, 数学)
src/                     実装
bin/                     img2cpp, gltf2cpp, dbones2cpp とその依存パッケージ定義
library.json             PlatformIO マニフェスト
example/wasm/            demo2d, demo3d, demorig (WASM 版とネイティブ版のエントリポイント)
example/common/demorig/  demorig のシーン、ビュー、オーバーレイ (WASM 版と M5Stack 版で共用)
example/m5*/demorig/     M5Stack CoreS3 / Tab5 版の demorig (ESP-IDF プロジェクト)
example/m5common/        それらが共用する ESP-IDF コンポーネント (同ディレクトリの README 参照)
docs/                    公開サイト: デモページとその WASM ビルド
docsrc/                  マニュアルの Sphinx ソース。CI で /ref/ にデプロイ
                         (ローカルで読むには `make -C docsrc preview`)
test/                    自己検査型のテスト
```

## ライセンス

MIT。[LICENSE](LICENSE) を参照してください。同梱の Adafruit `gfxfont.h` (BSD)
の告知も同ファイルに含まれています。
