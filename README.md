# Poppy

Windows専用の、AI CLIと一緒に使う小さなコードビューアーです。左端にアイコンだけのアクティビティーバー、その右にエクスプローラー／Gitのサイドバー、さらに右に読み取り専用のコード表示、右下にターミナルを配置します。ファイルの編集はターミナル内のAIに任せる構成です。

C++17 / Win32 / GDI / ConPTYで実装し、ブラウザー、WebView、Node.js、.NETランタイム、エディターエンジンを常駐させません。LSP、拡張機能、編集機能は入れていません。

**初期実装です。Windows上での動作、実際のAI CLIとの互換性、メモリ使用量は未検証です。** Docker上でのWindows向けクロスビルドとLinux用コアテストは実行済みです。メモリ使用量の達成値は記載していません。

## ビルド

生成するアプリはWindows 10 version 1809以降のx64環境が対象です。Git表示には、別途Git for WindowsをPATHに追加してください。AI CLIも利用するものを別途インストールします。

### Dockerでビルド

GNU Makeを用意し、Docker DesktopをLinuxコンテナモードで起動して、リポジトリのルートで実行します。ホスト側のVisual StudioやCMakeは不要です。

```sh
make dev
make artifact
```

`make dev`は`docker compose up -d --build dev`を実行し、`poppy-dev`コンテナを常駐させます。Dockerの再起動後も自動起動します。明示的に停止した場合は`make dev`で再開できます。

`make artifact`は`dev`を起動した後、`docker compose exec -T dev /usr/local/bin/poppy-build`を実行します。Docker内でLinux用のコアテストを実行した後、MinGW-w64でWindows x64向けReleaseビルドを行い、`out/docker/Poppy.exe`に出力します。ビルド終了後もコンテナは起動したままです。C++とwinpthreadsのランタイムは静的リンクします。生成した実行ファイルをWindows上で起動してください。

```powershell
.\out\docker\Poppy.exe "C:\work\project"
```

初回のイメージ作成にはDocker HubとDebianパッケージ配布元への接続が必要です。イメージ作成後のテスト・コンパイルは、外部接続できない内部ネットワーク（`internal: true`）で実行します。ソースを読み取り専用でマウントし、中間生成物はComposeの`build-cache`ボリューム、完成した実行ファイルはホストの`out/docker`に保存します。並列コンパイル数は既定で2、`POPPY_BUILD_JOBS`環境変数で変更できます。

`docker/build.sh`はイメージ作成時に`/usr/local/bin/poppy-build`へコピーし、実行権限`0555`で固定します。コンテナのルートファイルシステムも読み取り専用にし、実行中のスクリプト差し替えを防ぎます。ビルドには書き込み可能な`/build`・`/output`と、一時ファイル用の`/tmp`（tmpfs）を使います。リポジトリ側の`docker/build.sh`を編集しても、ホストで`make dev`を実行してイメージを再ビルドするまで、許可済みの実行内容は変わりません。

コンテナを停止する場合は`docker compose stop dev`、削除する場合は`docker compose down`を実行します。中間生成物のキャッシュも破棄して作り直す場合は、`docker compose down --volumes`を実行します。旧`build`サービスから移行する場合は、`docker compose down --remove-orphans --volumes`で旧コンテナと旧マウント先を記録したCMakeキャッシュを削除してから、`make dev`を実行してください。

コンテナ内で実行するテストはLinux版のコアテストです。Windows実行ファイルの起動、GUI、ConPTYの動作確認はWindows実機で行います。

### AIコンテナからビルド・コンパイルチェック

ホスト側のWSL / Linuxでリポジトリのルートから`make dev`を実行し、`.aicontainer`に次の設定を追加します。

```ini
docker-proxy-name=poppy-docker-proxy
docker-proxy-containers=poppy-dev
docker-proxy-allow=sh /usr/local/bin/poppy-build
docker-proxy-allow=cmake
docker-proxy-allow=ctest
docker-proxy-allow=ninja
docker-proxy-allow=g++
docker-proxy-allow=x86_64-w64-mingw32-g++-posix
```

既存の`docker-proxy-allow=sh docker/build.sh`や`docker-proxy-allow=/usr/local/bin/poppy-build`は、上記の`sh`と固定パスを指定する設定に置き換えてください。設定を反映するため、AIコンテナを作り直して起動します。`.aicontainer`はローカル設定としてGit管理から除外されています。

AIコンテナ内でもリポジトリのルートから実行します。

```sh
# コアテストからWindows実行ファイルの生成まで実行
dp sh /usr/local/bin/poppy-build

# 初回ビルド後、Windows向けの差分コンパイルだけを実行
dp cmake --build /build/windows --parallel 2

# Linux用コアの差分コンパイルとテスト
dp cmake --build /build/linux --parallel 2
dp ctest --test-dir /build/linux --output-on-failure
```

Composeはホストの`PWD`と同じ絶対パスにソースをマウントします。`dp`はAIコンテナの作業ディレクトリをそのまま引き継ぐため、`dev`とAIコンテナでリポジトリの絶対パスを揃えてください。

### Windowsで直接ビルド

Visual Studio 2022の「C++によるデスクトップ開発」、Windows SDK、CMakeを用意し、Developer PowerShellで実行します。

```powershell
cmake -S . -B out -A x64 -DBUILD_TESTING=ON
cmake --build out --config Release
ctest --test-dir out -C Release --output-on-failure
.\out\Release\Poppy.exe "C:\work\project"
```

フォルダー指定を省略すると空のウィンドウが開きます。Release版はCRTを静的リンクし、配布物は`Poppy.exe`単体です。通常利用にVisual StudioやCMakeは必要ありません。

### コアテストだけを直接実行

Windows以外でも、ポータブルなテキスト処理・ターミナルモデルのテストをネイティブビルドできます。

```sh
cmake -S . -B out -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build out
ctest --test-dir out --output-on-failure
```

## 操作

| 操作 | 動作 |
| --- | --- |
| Open folder / Ctrl+O | 作業フォルダーを開く |
| アクティビティーバーのファイルアイコン / Ctrl+Shift+E | サイドバーをエクスプローラーに切り替える |
| アクティビティーバーのGitアイコン / Ctrl+Shift+G | サイドバーをGitの変更ファイル一覧に切り替える |
| ファイルを選択 | 読み取り専用で表示する |
| Gitの変更ファイルを選択 | 差分を表示する。未追跡ファイルは通常表示する |
| ビューアーでドラッグ → Ctrl+C | コードを選択してコピーする |
| Ctrl+A | ビューアーの全テキストを選択する |
| Refresh / F5 | ツリー、Git、開いているファイルを再取得する |
| Diff / F6 | 選択ファイルのGit差分と通常表示を切り替える |
| Terminal / Ctrl+` | ターミナルの表示を切り替える |
| ターミナルをクリック | 初めて操作した時点でPowerShellを起動する |
| ターミナルでCtrl+Shift+V / 右クリック | ペーストする |
| ターミナルでドラッグ → Ctrl+Shift+C | 表示内容を選択してコピーする |
| ターミナルでCtrl+C | 実行中のコマンドへCtrl+Cを送る |
| ターミナルでホイール | 上限内の履歴を表示する |
| 境界をドラッグ | サイドバー幅・ターミナル高さを調整する |

ターミナル内では、普段使う`codex`や`claude`などのコマンドを直接実行します。プロバイダーのAPIキーをアプリ側で管理する機能はありません。PowerShellは`-NoProfile`で起動します。

アクティビティーバーのアイコンでサイドバーを切り替えます。選択中のアイコンには左端に目印が付き、アイコンにマウスを重ねると名前とショートカットを表示します。エクスプローラーの展開状態は切り替えても保持します。

Gitはブランチ、変更数、未追跡ファイル数、ツリー上の状態記号を表示します。Gitサイドバーでは状態記号とファイルパスを一覧表示します。`M`は変更、`A`は追加、`D`は削除、`R`はリネーム、`U`は競合、`?`は未追跡です。削除済みファイルもGit一覧から差分を開けます。未追跡ディレクトリの中もファイル単位で一覧に含めます。差分はまず作業ツリーを表示し、差分がなければステージ済みの差分を表示します。未追跡ファイルは通常のファイル表示を使います。コミット・ステージ操作はターミナルで行います。

AIなどによるフォルダー内の変更を検知し、500 msの待ち合わせ後に再取得します。変更が続く場合は約2秒ごとに更新を依頼します。OS通知が利用できない場合や作業フォルダー外での変更は、F5で更新できます。読み込み中の新しい結果が届くまで時間がかかる場合があります。

別のフォルダーを開いても、実行中のターミナルはそのまま残します。ターミナルの見出しに起動時のフォルダーを表示します。必要に応じて`cd`してください。アプリを終了すると、そのターミナルのプロセスと子プロセスも終了します。

## メモリを抑える構成

- ファイルツリーは展開した階層だけ列挙し、折りたたんだ配下のノードを解放します。`.git`は表示せず、ディレクトリのリンク・ジャンクションは再帰展開しません。
- 表示ファイルは1つだけ保持します。ファイル全文のUTF-8スナップショットと行頭位置・行ごとの字句状態を保持し、表示行のトークンと描画用文字列をその場で作ります。非表示タブ、全ファイルの索引、全行の描画キャッシュはありません。
- ディスクI/OとGitは別のワーカーで処理し、UIスレッドでファイル走査やGitの終了待ちを行いません。古い読み込み結果を表示しないよう世代と要求番号を確認します。
- シェルは操作時に起動します。ターミナルは固定セルの画面と上限付き履歴を持ち、外部ターミナルアプリを埋め込む構成にはしません。
- 大量のターミナル出力は1回8 KiBずつ処理し、約16 ms間隔で続きを処理します。表示が追いつかない場合は出力側を待機させ、メモリ増加とUIの占有を抑えます。
- 待機中はWindowsのメッセージとディレクトリ変更イベントを待ちます。変更通知の待ち合わせとターミナル出力の処理中だけタイマーを使用します。Gitの常時ポーリングは行いません。

| 項目 | 初期上限 |
| --- | --- |
| ファイル | 読み込み時・UTF-8変換後ともに16 MiB / 1,000,000行 |
| 長い行の表示 | 先頭8,192桁まで。コピー対象には全文が残る |
| ファイルツリー | 1フォルダー10,000件 / 全体50,000ノード |
| Git変更ファイル一覧 / 状態のバッジ | 50,000ファイル / 親ディレクトリを含め50,000パス |
| Gitコマンドの出力 | 1回4 MiB / 差分は1,000,000行まで |
| ターミナル画面 | 最大500列 × 200行 × 通常・代替の2画面 |
| ターミナル履歴 | 最大1,000行、かつ131,072セル。セル領域は約2 MiB以内 |
| ターミナル受信待ち | 256 KiB。超過時はパイプ読み取りを待機する |
| ターミナル送信待ち | 1 MiB。超過する入力は受け付けない |

これらはアプリ全体のメモリ上限ではありません。読み込み中の旧・新ファイル、UTF-16変換、コピー、Gitの結果入れ替えなどで一時的な追加領域を使用します。Windowsの共有DLLやツリーコントロールの領域も必要です。PowerShell・AI CLI・Gitは別プロセスでメモリを使います。

## 初版の表示範囲

ファイル表示はUTF-8、BOM付きUTF-16 LE / BEに対応します。バイナリーやShift_JISなどはプレビュー対象外です。簡易シンタックスハイライトはC/C++、C#、JavaScript/TypeScript、Rust、Go、JavaなどのC系構文、Python、シェル、JSON、TOML/YAML/INI、Git差分を対象とします。言語の完全な構文解析は行いません。

ターミナルはConPTYのUTF-8出力を受け取り、基本的なカーソル移動、行・文字の挿入と削除、スクロール領域、代替画面、16/256/true color、bracketed paste、カーソル・背景色の照会に対応する小さなVT実装です。日本語などの全角文字とIMEの確定文字列を扱うコードを含みます。

Windows Terminal相当の互換性はまだありません。結合文字・ZWJ絵文字の合成、文字装飾の完全再現、マウスプロトコル、画像、Kittyキーボードプロトコル、履歴の折り返し再計算は未対応です。使うAI CLIで表示と操作を確認する必要があります。

## 検証と計測

`tests/core_tests.cpp`は、エンコーディング、不正・分割UTF-8、複数行ハイライト、分割VTシーケンス、色、全角文字の上書き、画面切り替え、スクロール領域、入力モード、履歴上限、リサイズを検証するテストです。Docker上のLinux環境で実行し、通過を確認しました。

Windowsで`BUILD_TESTING=ON`にしてビルドすると、`ctest`で`tests/folder_tests.cpp`も実行します。日本語・空白を含む一時フォルダーでPoppyを起動し、フォルダーを開いた直後の表示、子フォルダーの展開・折りたたみ・再展開、空のフォルダーやワークスペースへのファイル追加後の表示を検証します。アクティビティーバー・サイドバー・コード表示の配置、サイドバー切り替え、切り替え中の更新、境界のドラッグも検証します。テスト中はPoppyのウィンドウが開くため、デスクトップを利用できるWindows環境で実行してください。

WindowsではReleaseビルド後、次の順で確認します。

1. フォルダー展開・折りたたみ、ファイル切り替え、スクロール、選択とコピーを確認する。
2. 小さいファイル、日本語ファイル、空ファイル、16 MiB超のファイル、非常に長い行を表示する。
3. Gitの未追跡・変更・ステージ済み・競合・リネームの表示と差分を確認する。
4. AI CLIを起動し、日本語IME、ペースト、Ctrl+C、代替画面、リサイズ、継続的な大量出力を確認する。
5. 外部でファイルを変更・作成・削除・置換し、表示更新を確認する。
6. 大量出力中、ペースト中、Git実行中にアプリを閉じ、停止や子プロセスの残留がないことを確認する。
7. 100% / 150% / 200%表示倍率とモニター間移動を確認する。

メモリ計測用スクリプトも用意しています。

```powershell
$app = Start-Process .\out\Release\Poppy.exe -PassThru
.\scripts\measure-memory.ps1 -ProcessId $app.Id -Samples 30 -CsvPath idle.csv
# フォルダー・ファイルを開いた後にも同じように計測する
.\scripts\measure-memory.ps1 -ProcessId $app.Id -Samples 30 -CsvPath viewer.csv
# AI CLIを動かした状態で子プロセスも含めて計測する
.\scripts\measure-memory.ps1 -ProcessId $app.Id -Samples 30 -IncludeChildren -CsvPath with-ai.csv
```

Working Setはその時点で物理メモリに載っている領域、Private Bytesはプロセス専用のコミット量です。スクリプトは両方の平均とサンプル内最大値を出します。瞬間的なピークを取りこぼす場合があります。Poppy単体と、AI CLIなどを含む合計を分けて記録してください。計測のためにWorking Setを強制的に削る処理は行いません。
