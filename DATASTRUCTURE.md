# 保存データの構造

Poppyは、アプリ全体の状態とプロジェクトごとの状態を別々のSQLite3データベースに保存します。各データベースは最新の状態を保持し、過去の状態の履歴は保存しません。

## 保存先

```text
%APPDATA%\yumayo\Poppy\
├── storage.sql                         # アプリ全体のデータベース
└── projects\
    └── -d-repository-yumayo-poppyworktrees-poppydirect2d\
        └── storage.sql                 # このプロジェクトのデータベース
```

`APPDATA` が未設定の場合は、WindowsのRoaming AppDataフォルダーを使います。拡張子は `.sql` ですが、ファイルの中身はSQLスクリプトではなくSQLite3のデータベースです。

パスなどの文字列はUTF-8の `TEXT`、数値と真偽値は `INTEGER` で保存します。真偽値は `0` がfalse、`1` がtrueです。ファイルやフォルダーのパスは絶対パスで保持します。

## プロジェクトの識別とディレクトリー名

プロジェクトの識別には、開いたフォルダーの絶対パスを使います。

1. `.`、`..` などを字句的に正規化し、区切り文字をWindows形式に統一する。
2. 小文字化し、ドライブのルート以外では末尾の区切り文字を取り除く。この値を `path_key` とする。
3. `path_key` から、保存先のディレクトリー名を生成する。

シンボリックリンクやジャンクションの参照先を解決する処理は行っていません。

| 項目 | 例 |
| --- | --- |
| 開いたフォルダー | `D:\repository\yumayo\PoppyWorktrees\PoppyDirect2D` |
| `path_key` | `d:\repository\yumayo\poppyworktrees\poppydirect2d` |
| 保存先のディレクトリー名 | `-d-repository-yumayo-poppyworktrees-poppydirect2d` |

ディレクトリー名は `-` から始まり、英小文字・数字・アンダースコアを残し、それ以外の文字をハイフンに置き換えます。連続するハイフンはまとめ、末尾のハイフンは取り除きます。

名前が120文字を超える場合は、先頭120文字に `-` とハッシュを付けます。また、別のプロジェクトと名前が衝突した場合もハッシュを付け、必要なら連番を追加します。ハッシュは `path_key` のUTF-8バイト列から計算するFNV-1aの64ビット値で、16桁の小文字の16進数です。

一度割り当てた名前は全体データベースの `projects` テーブルで保持します。対応が登録されていない既存ディレクトリーは再利用せず、別名を割り当てます。

## アプリ全体のデータベース

ファイル: `%APPDATA%\yumayo\Poppy\storage.sql`

### `projects`: プロジェクトと保存先の対応

1プロジェクトにつき1行です。

| カラム | 型・制約 | 内容 |
| --- | --- | --- |
| `path_key` | `TEXT PRIMARY KEY` | 正規化・小文字化したプロジェクトの識別用パス |
| `path` | `TEXT NOT NULL` | 登録時のプロジェクトの絶対パス |
| `directory` | `TEXT NOT NULL UNIQUE` | `projects\` 配下のディレクトリー名。親のパスは含まない |

`directory` を使ってプロジェクトのデータベースを開きます。全体のデータベースとプロジェクトのデータベースを結ぶSQLの外部キーはありません。

### `open_projects`: 開いている一覧（全体形式2）

`path_key TEXT PRIMARY KEY`、`path TEXT NOT NULL`、`position INTEGER NOT NULL UNIQUE` を持ちます。`projects` は最近使った保存先の対応表、こちらは現在開いている一覧です。保存時は全行をトランザクション内で置き換えます。`app.project_list_width INTEGER NOT NULL DEFAULT 180` は96 DPI基準の一覧幅です。

### `app`: 選択プロジェクトとウィンドウ配置

`id = 1` の最大1行です。`id` には `PRIMARY KEY` と `CHECK(id=1)`、その他のカラムには `NOT NULL` を設定しています。

| カラム | 型 | 内容 |
| --- | --- | --- |
| `id` | `INTEGER` | 固定値 `1` |
| `last_project` | `TEXT` | 前回保存したプロジェクトの絶対パス。未選択時は空文字列 |
| `x` | `INTEGER` | 通常表示時のウィンドウ左端。画面座標のピクセル値 |
| `y` | `INTEGER` | 通常表示時のウィンドウ上端。画面座標のピクセル値 |
| `width` | `INTEGER` | 通常表示時のウィンドウ幅。ピクセル値 |
| `height` | `INTEGER` | 通常表示時のウィンドウ高さ。ピクセル値 |
| `positioned` | `INTEGER` | 保存済みのウィンドウ配置がある場合に `1` |
| `maximized` | `INTEGER` | 最大化していた場合に `1`。最小化中は最大化へ戻す設定も反映する |

初回起動などで行がない場合、前回のプロジェクトは未選択、ウィンドウサイズは1380×820として起動します。

## プロジェクトごとのデータベース

ファイル: `%APPDATA%\yumayo\Poppy\projects\<directory>\storage.sql`

### `session`: ペイン配置、選択状態、差分とGit一覧の表示形式

`id = 1` の最大1行です。`id` には `PRIMARY KEY` と `CHECK(id=1)`、その他のカラムには `NOT NULL` を設定しています。

| カラム | 型 | 内容 | 初回の値 |
| --- | --- | --- | --- |
| `id` | `INTEGER` | 固定値 | `1` |
| `sidebar` | `INTEGER` | サイドバー幅。96 DPIを基準にした論理ピクセル値 | `245` |
| `terminal_height` | `INTEGER` | ターミナル高さ。同じく96 DPI基準 | `260` |
| `terminal_visible` | `INTEGER` | ターミナルを表示する場合に `1` | `1` |
| `git_sidebar` | `INTEGER` | SOURCE CONTROLなら `1`、BRANCHなら `2`、エクスプローラーなら `0` | `0` |
| `active_tab` | `INTEGER` | 選択中のタブの、0から始まる順序 | `0` |
| `selected_path` | `TEXT` | エクスプローラーで選択していたファイルまたはフォルダーの絶対パス | 空文字列 |
| `diff_inline` | `INTEGER` | プロジェクト共通の差分表示形式。Inline表示なら `1`、Split表示なら `0` | `1` |
| `git_tree` | `INTEGER` | Git変更一覧の表示形式。Tree表示なら `1`、List表示なら `0` | `1` |

「初回の値」はアプリ側の初期値です。新規作成時のSQLには `DEFAULT` 制約を設定していません。旧形式から移行したデータベースの `diff_inline` と `git_tree` には `DEFAULT 0` を設定します。表示時には現在のDPIとウィンドウサイズに合わせてペインの寸法を調整します。差分表示形式はファイルを切り替えても共通で使い、すべてのタブを閉じても保持します。Git一覧の表示形式はプロジェクトごとに保持し、フォルダーの折りたたみ状態は `git_collapsed` に保存します。

### `tabs`: 開いていたタブ

1タブにつき1行です。`position` が主キーで、その他のカラムには `NOT NULL` を設定しています。

| カラム | 型 | 内容 |
| --- | --- | --- |
| `position` | `INTEGER PRIMARY KEY` | 0から始まるタブの順序 |
| `path` | `TEXT` | ファイルの絶対パス |
| `diff` | `INTEGER` | 差分表示なら `1`、通常表示なら `0` |
| `file_top` | `INTEGER` | 通常表示の先頭にある元の行。0から始まる |
| `file_wrap_offset` | `INTEGER` | 先頭の折り返し行の、元の行内でのUTF-8バイトオフセット |
| `file_left` | `INTEGER` | 通常表示の横スクロール位置。文字セル単位 |
| `file_anchor` | `INTEGER` | 通常表示の選択開始位置 |
| `file_caret` | `INTEGER` | 通常表示の選択終了位置 |
| `file_after` | `INTEGER` | 通常表示のビュー状態に含まれるフラグ。通常表示では使用しない |
| `diff_top` | `INTEGER` | 差分表示の先頭表示行。削除行と追加行を区別できるInline表示を基準にした0から始まる行位置。Split表示では対応する行へ変換する |
| `diff_left` | `INTEGER` | 差分表示の横スクロール位置。文字セル単位 |
| `diff_anchor` | `INTEGER` | 差分表示の選択開始位置 |
| `diff_caret` | `INTEGER` | 差分表示の選択終了位置 |
| `diff_after` | `INTEGER` | 左右比較での選択側。変更前が `0`、変更後が `1` |
| `pinned` | `INTEGER` | 固定したタブなら `1`、通常のタブなら `0` |

選択位置はUTF-8本文のバイトオフセットです。差分表示では内部のGitパッチ本文に対するオフセットになります。通常表示と差分表示を行き来しても、それぞれの表示位置と選択範囲を独立して復元できます。

固定タブは上段の専用行に並べ、通常タブはその下の行から配置します。それぞれ幅に収まらない場合は複数行に折り返します。固定状態と順序を保存し、固定を解除するまで閉じる操作を受け付けません。旧形式から移行したタブの `pinned` は `DEFAULT 0` とします。

ファイル本文やGit差分の本文は保存せず、タブを表示するときに読み直します。

### `expanded`: エクスプローラーの展開状態

展開していたフォルダーにつき1行です。

| カラム | 型・制約 | 内容 |
| --- | --- | --- |
| `path` | `TEXT PRIMARY KEY` | 展開していたフォルダーの絶対パス |

プロジェクトのルートフォルダーも対象です。ルートを折りたたんだ場合は、その行と配下の行がなくなります。新規プロジェクトではルートを展開します。

## 追加のビュー状態（プロジェクト形式5）

`view_state` は `id=1` の1行です。`explorer_top` / `explorer_left`、`git_top` / `git_left` は各ツリーのスクロール位置、`first_tab_row` はタブ領域の表示段、`git_selected` はGitの選択パス、`focused_pane` は `viewer` / `explorer` / `git` / `branch` / `terminal`、`active_terminal` は選択ターミナルのIDです。数値は `INTEGER NOT NULL`、パス・ペイン・IDは `TEXT NOT NULL` です。

`terminals` は `position INTEGER PRIMARY KEY`、`terminal_id TEXT NOT NULL UNIQUE`、`name TEXT NOT NULL`、`directory TEXT NOT NULL` を持ちます。`directory` は最初の起動フォルダーであり、シェル内部の現在位置ではありません。`git_collapsed(path TEXT PRIMARY KEY)` はGitツリーの折りたたみパスです。

プロセスハンドル、ConPTY、入出力、端末画面、AI状態、通知ID、未確認通知は保存しません。通知IDはターミナルの実行開始ごとに生成し、保存用の `terminal_id` と区別します。ターミナルは最大100件、開くプロジェクトは最大1,000件です。

## 保存と復元の流れ

### 保存

操作したプロジェクトをdirtyにし、最後の入力から650 ms後にスナップショットを専用保存ワーカーへ渡します。同じ対象の未処理の保存要求は最新値にまとめます。SQLite接続は保存ワーカーが所有します。切り替え時はメモリ上の表示位置と入力先だけを確定し、保存を待ちません。通常の終了時（`WM_CLOSE`）とWindowsの終了通知時（`WM_QUERYENDSESSION`）は、非表示プロジェクトも保存して待機中の保存を完了させます。

1. 表示中のタブの位置・選択範囲をメモリ内のタブ状態に反映する。
2. プロジェクトのデータベースで `BEGIN IMMEDIATE` を実行する。
3. `tabs`、`expanded`、`terminals`、`git_collapsed` を置き換える。`session` と `view_state` はそれぞれ1行を置き換える。
4. `COMMIT` する。途中で失敗した場合は `ROLLBACK` し、前回のプロジェクト状態を残す。
5. 全体のデータベースの `app` を置き換え、選択プロジェクトとウィンドウ配置・一覧幅を保存する。同じ全体DBトランザクションで `open_projects` の一覧と順序も置き換える。

全体のデータベースとプロジェクトのデータベースは別々に更新します。両方をまとめた単一のトランザクションにはしていません。SQLiteのロック待ち時間は各接続で1000 msです。

### 復元

1. 起動時に全体のデータベースからウィンドウ配置を読む。保存位置が現在の画面に収まらない場合は表示可能な範囲に調整する。
2. `open_projects` の一覧を復元し、コマンドラインのフォルダーを追加または既存選択する。指定がなければ `last_project` を選ぶ。選択プロジェクトの読み込みを優先し、存在しないフォルダーは一覧へ残す。
3. `projects` の対応表からプロジェクトのデータベースを開き、`session`、`tabs`、`expanded` を読み取る。プロジェクト状態の読み取りには同一トランザクション内のスナップショットを使う。
4. ペイン配置とタブの順序を復元する。削除された通常表示のファイルは除外し、残ったタブに合わせて選択を調整する。削除ファイルの差分タブは残す。
5. エクスプローラーの各階層を非同期で読み込みながら、展開状態と選択を復元する。
6. 選択タブの本文を読み、スクロール位置と選択範囲を復元する。差分タブはGitの読み込みを待ってから表示する。

ターミナルはID・順序・名前・起動フォルダー・選択と表示・高さを復元します。実行中だったプロセスや出力履歴は保存せず、次の操作時に新しいPowerShellを起動します。

## 保存形式のバージョンと上限

現在の形式は全体のデータベースが `PRAGMA user_version = 3`、プロジェクトのデータベースが `PRAGMA user_version = 9` です。未初期化のデータベースではテーブルを作成してバージョンを設定します。それぞれの対応バージョンより `user_version` が大きい場合は、そのデータベースの読み込み・保存を拒否し、形式を上書きしません。

プロジェクトの旧形式（バージョン1）はトランザクション内で自動移行します。選択中の差分タブの形式を `session.diff_inline` に引き継ぎます。選択中のタブが通常表示の場合は先頭の差分タブの形式を使い、差分タブがなければSplit表示を使います。タブごとの `file_inline` と `diff_inline` は削除します。旧Split表示の行位置は元のパッチなしでは変換できないため、その差分の縦スクロール位置だけを先頭に戻します。その他のタブ状態とプロジェクト設定は引き継ぎます。

バージョン2から3への移行では `session.git_tree` を追加し、従来のList表示を初期値として引き継ぎます。バージョン3から4への移行では `tabs.pinned` を追加し、既存タブは固定しない状態で引き継ぎます。バージョン4から5では `view_state`、`terminals`、`git_collapsed` を追加します。旧形式は未起動のシェルを1件用意し、既存の表示・高さを維持します。全体DBの1から2では `app.project_list_width` と `open_projects` を追加し、旧 `last_project` だけを最初の1件として引き継ぎます。最近使った `projects` の全行を開くことはしません。移行はそれぞれ同じDBトランザクション内で行います。

| 項目 | 制限 |
| --- | --- |
| 保存・読み込み対象のタブ | 最大10,000件 |
| 保存・読み込み対象の展開フォルダー | 最大50,000件 |
| 読み込み時の縦スクロール位置 | 0〜1,000,000行 |
| 読み込み時の横スクロール位置 | 0〜8,192文字セル |
| 読み込み時の選択位置 | 0〜16 MiBのバイトオフセット |

保存件数が上限を超える場合は、前回の状態を更新せずエラーにします。実際の表示位置と選択範囲は、本文を読み直した後、その時点のファイルサイズや表示範囲にも合わせて調整します。

読み込み・保存のエラーは画面に表示します。全体のデータベースを初期化・読み込みできなかった場合は状態保存を無効にして起動し、プロジェクトの状態を読み込めなかった場合は、そのプロジェクトの状態を上書きしません。

## 実装とテスト

| ファイル | 役割 |
| --- | --- |
| [src/session.h](src/session.h) | `AppSession`、`ProjectSession`、`SavedTab`、`ViewState` のメモリ上の構造 |
| [src/session.cpp](src/session.cpp) | 保存先のディレクトリー名とハッシュの生成 |
| [src/storage.h](src/storage.h)・[src/storage.cpp](src/storage.cpp) | SQLite接続、スキーマ、保存・読み込み、保存先の対応表 |
| [src/workspace.cpp](src/workspace.cpp)・[src/app.cpp](src/app.cpp) | 一覧・保存ワーカー、プロジェクトごとの状態取得と復元 |
| [tests/session_tests.cpp](tests/session_tests.cpp) | ディレクトリー名、長いパス、衝突に使うハッシュの検証 |
| [tests/storage_tests.cpp](tests/storage_tests.cpp) | 日本語パスを含む保存と再読み込み、プロジェクトの分離、衝突、ロールバック、保存形式の保護 |
| [tests/folder_tests.cpp](tests/folder_tests.cpp) | Windows上での再起動後のタブ・表示位置・ツリー展開・ペイン配置の復元 |

## 左右分割（プロジェクトDB version 6）

version 5からの移行では既存のタブ・ターミナルを保ち、以下のテーブルを追加します。旧状態は1グループ・幅比500 / 1000として復元します。

| テーブル | カラム | 用途 |
| --- | --- | --- |
| split_layout | id=1, editor_ratio, terminal_ratio, right_tab_row | 左側の幅（200〜800 / 1000）、右Editorの先頭タブ行 |
| tab_groups | path PRIMARY KEY, pane, displayed | ファイルタブの所属（0=左、1=右）、グループ内の選択 |
| terminal_groups | terminal_id PRIMARY KEY, pane, displayed | ターミナルの所属と選択 |

既存のactive_tabとactive_terminalは入力先を表します。左右で各1タブを同時表示し、空のグループは統合します。分割状態も同じSQLiteトランザクションで保存し、未保存のEditor本文はDBに書き込みません。

## WSL接続（全体DB version 3 / プロジェクトDB version 7）

Windowsプロジェクトの識別キーと既存の保存先名は維持します。WSLの識別キーは、接続種別、登録ディストリビューション、要求ユーザー、POSIXパスを長さ付きのタプルにします。Linuxパスのケース・Unicode・バックスラッシュは保持し、Windowsパス用の正規化を適用しません。WSLの保存先名は `wsl-<distro>-<readable-root>-<hash>` です。ユーザーを含む完全なキーからハッシュを求め、衝突時は既存のレジストリで別名を割り当てます。

全体DBの `projects` に `kind INTEGER NOT NULL DEFAULT 0`、`distro TEXT NOT NULL DEFAULT ''`、`user TEXT NOT NULL DEFAULT ''`、`remote_root TEXT NOT NULL DEFAULT ''` を追加します。`kind=0` はWindows、`kind=1` はWSLです。`path`、`open_projects.path`、`app.last_project` はWSLの場合 `poppy-wsl:<distroのhex>:<userのhex>:<rootのhex>` を保持します。これは保存用の識別表現で、画面にはLinuxパスとディストリビューション名を表示します。

プロジェクトDBには `connection(id INTEGER PRIMARY KEY CHECK(id=1), kind INTEGER NOT NULL, distro TEXT NOT NULL, user TEXT NOT NULL, remote_root TEXT NOT NULL, uid TEXT NOT NULL)` を追加します。前回接続したUIDを照合し、既定ユーザーが変わった場合に同じ権限の接続として再開しません。タブ・展開・選択・ターミナルのcwdは生のPOSIXパスを保持し、左右分割はversion 6のモデルを再利用します。

移行は同一DBのトランザクション内で実行します。接続中フラグ、要求ID、PID、未保存本文、AIの実行状態は保存しません。非選択のWSLプロジェクトは構成だけを復元し、ユーザーが選択してから接続します。接続処理と配布物は [WSL対応](docs/wsl-support.md) を参照してください。

## Git比較タブ（プロジェクトDB version 8）

version 7からの移行で`git_comparisons`を追加します。`position INTEGER PRIMARY KEY`は`tabs.position`に対応し、`kind INTEGER NOT NULL`は通常のF6比較（0）、Changes（1）、Staged Changes（2）、コミット（3）を表します。`revision TEXT NOT NULL`はコミットID、`all_changes INTEGER NOT NULL`は全ファイル比較のフラグです。`pane INTEGER NOT NULL`と`displayed INTEGER NOT NULL`で左右の所属と選択を保持します。同じパスを複数の比較対象やコミットで開くことができ、各比較の位置も独立して復元します。旧形式の`tab_groups`は通常タブの互換情報として残し、version 8では位置ごとの比較情報を優先します。

version 9では`tabs.file_wrap_offset`を追加します。元の行番号と行内のバイト位置を保存し、ウィンドウ幅が変わっても対応する折り返し行へ復元します。旧形式からの移行時は`DEFAULT 0`として元の行の先頭を表示します。

`session.git_sidebar`は0=EXPLORER、1=SOURCE CONTROL、2=BRANCHです。`git_collapsed.path`と`view_state.git_selected`はグループを区別する`staged:`または`changes:`を相対パスの前に付けて保存します。BRANCHのコミット・変更ファイルのツリーは必要時にGitから取得し、保存しません。
