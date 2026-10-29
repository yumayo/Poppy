# WSLのメモリと上限エラー

2026-10-28調査。`src/`内で`exceed`を含むエラー文字列は**25箇所・23種類**です。このうちWSLのクライアント・ヘルパー・通信プロトコルに10箇所あります。これはコード中の出現数で、利用者の環境でのエラー発生回数ではありません。`exeed`という綴りの文字列はありません。

## メモリ増加と切断の関係

利用者が覚えていたエラー原文は`WSL listing exceeds the receive limit`です。これにより、蓄積型の一覧受信で上限超過を検出して接続全体を停止したことが確認できました。この文字列だけでは件数とバイト数のどちらを超えたかは分かりません。ただし、この版のヘルパーはファイル一覧を20万件で打ち切り、クライアントも20万件ちょうどまでは受け付けるため、正常なヘルパーとの通信では接続合計64 MiBの超過が有力です。超過時の件数・バイト数・操作名は未取得です。

WSLプロジェクトを開くと、Ctrl+P用のファイル一覧収集とGitリポジトリ検出を開始します。ファイル一覧は`.git`の内部とディレクトリへのリンクを除外しますが、`.gitignore`の対象も収集します。`node_modules`、`vendor`、ビルド出力などが多いと、Gitがなくても大量になります。

修正前のWindows側クライアントは、一覧の各項目を`std::map<std::string, std::string>`として全件保持し、受信完了後に検索用の相対パスとUTF-32の名前へ変換していました。通信バイト数に加え、各辞書のノード・文字列・変換先の一覧が必要です。受信中の一覧・本文の合計が同じディストリビューション・要求ユーザーの接続で64 MiBを超えると、受信スレッドが例外を送出し、共有する接続全体を停止します。別プロジェクトも切断される理由になります。

今回の変更では、ファイル一覧を1件ずつ検索用データへ変換し、全件分のJSON辞書を保持しません。保持する一覧には、文字列容量とベクター拡張分を含む64 MiB・20万件の予算を設けました。この予算に達したら以降の項目を保持せず、応答完了まで受信を続けます。収集済みの一覧は検索でき、Ctrl+Pの下部に制限を表示します。この一覧の予算到達だけでは接続を停止しません。再収集中は前回の一覧もあるため、Poppy.exe全体のメモリが64 MiB以下になるという意味ではありません。

切断時には、ステータスとプロジェクトのツールチップに接続のエラー原文を残します。

送信側も、完成した一覧フレームをエンコード後の合計64 KiBを目標にまとめて出力するよう変更しました。件数での制限はなく、次のフレームが収まらなければ現在の分を送信します。端数は完了・エラー時に送信し、検索の途中結果は従来の結果群ごとに送り出します。これは送信バッファーの64 **KiB**で、受信中のデータ合計や保持する検索用一覧の64 **MiB**とは別の制限です。詳細は [WSL対応](wsl-support.md) を参照してください。

Gitの出力上限4 MiBは、通常はそのGit操作だけを失敗させます。ただし、大きなフォルダーのリポジトリ検出・監視登録も並行するため、Gitのあるプロジェクトで目立つ可能性はあります。従来は監視が8,192件に達した場合や`/mnt/`配下で、変更の有無に関係なく2秒ごとに全体の再検証通知を送り、一覧やGitの再収集が発生していました。

2026-10-29の追加調査で、この無条件の通知に加え、WSLのGit内部更新も全ファイル一覧を無効にすること、Git更新のたびにリポジトリ検出から繰り返すこと、プロジェクトへの復帰とフォルダーの再展開でも再取得することを確認し、修正しました。WindowsのGit通知フィルターも入れ子の`.git`と参照のロックに対応させています。監視が不足する場合は制限を表示し、通知されない変更にはF5を使います。読み込み済みの子項目は保持し、構成変更時に無効化します。

64 MiBは接続中の累計通信量ではなく、同時に受信・保持している本文と蓄積型一覧の上限です。ストリーム処理する索引の累計通信量が64 MiBを超えるだけでは異常とは限りません。一方、無変更のフォルダーで同じ一覧を繰り返し転送する挙動は修正対象です。大きな素のリポジトリで再現しなくても、依存パッケージなどでディレクトリ数が増えた環境では監視上限に達する可能性があります。

## `exceed`を含む全エラー

末尾のピリオドも原文どおりです。同じ文字列の発生箇所は同じ行にまとめています。

### WSLで発生するもの

| エラー原文 | 発生条件・影響 | 発生元 |
| --- | --- | --- |
| `WSL listing exceeds the receive limit` | 蓄積型の一覧が20万件を超えるか、接続の受信保持量が64 MiBを超過。接続全体を停止。修正後のCtrl+P一覧は蓄積型を使わない | [wsl_client.cpp](../src/wsl_client.cpp) `Connection::received` |
| `WSL responses exceed the connection receive limit` | 受信本文追加時に、接続の受信保持量が64 MiB超過。接続全体を停止 | [wsl_client.cpp](../src/wsl_client.cpp) `Connection::received` |
| `WSL file exceeds the 16 MiB receive limit` | ヘルパーの応答ヘッダーが16 MiB超の本文を宣言。接続全体を停止。通常の大きなファイルはヘルパー側で拒否する | [wsl_client.cpp](../src/wsl_client.cpp) `Connection::received` |
| `WSL search exceeded its result budget` | ヘルパーからの検索結果が2,000件または概算2 MiB超過。受信コールバックの異常として接続全体を停止。正常時はヘルパーが上限で結果を打ち切る | [wsl_client.cpp](../src/wsl_client.cpp) `WslProvider::searchContent` |
| `File exceeds the 16 MiB limit` | ファイルの初期サイズ、または読み込み中に16 MiB超過。操作だけが失敗。2箇所で同じ文字列を使う | [wsl_helper.cpp](../src/wsl_helper.cpp) `read` |
| `Save exceeds the file or upload queue limit` | 保存・新規作成・検索除外データのアップロードで、単体16 MiBまたは未完了分の合計64 MiB超過。入力受付の異常としてヘルパーが終了し接続を失う | [wsl_helper.cpp](../src/wsl_helper.cpp) `Server::accept` |
| `Git output exceeds the 4 MiB limit` | Gitの標準出力と標準エラーの合計が4 MiB超過。Gitの子プロセス群を終了し、当該操作だけが失敗 | [wsl_helper.cpp](../src/wsl_helper.cpp) `Server::git` |
| `WSL control message exceeds 64 KiB` | JSON制御メッセージのエンコード結果が64 KiB超過。送信を開始せず例外。受信側の長さ違反は別の`Invalid WSL protocol data`になる | [wsl_protocol.cpp](../src/wsl_protocol.cpp) `encodeMessage` |
| `New file exceeds the 16 MiB limit.` | 新規作成の本文が16 MiB超過。作成操作だけを拒否。Windows用とWSL用の2箇所で同じ文字列を使う | [project_provider.cpp](../src/project_provider.cpp) / [wsl_client.cpp](../src/wsl_client.cpp) `createFile` |

### そのほか

以下はいずれも、当該表示・変換・ファイル操作のエラーです。直接WSL接続を停止する処理ではありません。

| エラー原文 | 発生条件 | 発生元 |
| --- | --- | --- |
| `This file exceeds the 1,000,000-line preview limit.` | 本文の改行が100万個以上 | [app.cpp](../src/app.cpp) |
| `Combined diff exceeds the 4 MiB preview limit.` | 複数ファイルの合計差分が4 MiB超過 | [app.cpp](../src/app.cpp) |
| `Diff exceeds the 1,000,000-line preview limit.` | 差分の改行が100万個以上 | [app.cpp](../src/app.cpp) |
| `Clipboard image exceeds the 64 MiB limit.` | DIBのクリップボードデータが64 MiB超過 | [clipboard_image.cpp](../src/clipboard_image.cpp) |
| `Clipboard bitmap pixels are truncated or exceed the 64 MiB limit.` | DIBの画素データが不足、または64 MiB超過 | [clipboard_image.cpp](../src/clipboard_image.cpp) |
| `Clipboard image is empty or exceeds the 64 MiB limit.` | クリップボード画像が空、または64 MiB超過 | [clipboard_image_win.cpp](../src/clipboard_image_win.cpp) |
| `Clipboard image dimensions exceed the 64 MiB pixel limit.` | 展開後の画素が64 MiB超過、縦・横が16,384ピクセル超過、またはサイズが無効 | [clipboard_image_win.cpp](../src/clipboard_image_win.cpp) |
| `PNG exceeds the 16 MiB file limit.` | 保存用PNGが空、または16 MiB超過 | [clipboard_image_win.cpp](../src/clipboard_image_win.cpp) |
| `Image exceeds the 16,384-pixel dimension or 64 MiB pixel limit.` | 画像の縦・横が16,384ピクセル超過、画素が64 MiB超過、またはサイズが無効 | [image_preview.cpp](../src/image_preview.cpp) |
| `Image is empty or exceeds the 16 MiB file limit.` | プレビュー画像の入力が空、または16 MiB超過 | [image_preview_win.cpp](../src/image_preview_win.cpp) |
| `This file exceeds the 16 MiB preview limit.` | Windowsの読み込み対象が16 MiB超過、または無効なサイズ | [project_provider.cpp](../src/project_provider.cpp) |
| `Save exceeds the 16 MiB limit.` | Windowsの保存データが16 MiB超過 | [project_provider.cpp](../src/project_provider.cpp) |
| `Decoded text exceeds the 16 MiB preview limit.` | デコード後のUTF-8本文が16 MiB超過 | [text.cpp](../src/text.cpp) |
| `Git output exceeds the 4 MiB limit.` | Windowsの外部プロセス出力が4 MiB超過。Git用エラー文を使い、当該プロセス群を終了 | [win.cpp](../src/win.cpp) `runProcess` |

## 同じ単語を使わないWSLの上限・時間切れ

| エラー原文、または表示動作 | 条件・影響 |
| --- | --- |
| `File list reached its file-count or memory limit.` | 今回追加。保持する検索用一覧が20万件または64 MiBの予算に到達。一覧を打ち切って検索を継続 |
| `WSL file list reached its 200,000-file or 50,000-directory limit.` | ヘルパーが走査上限を通知。一覧を打ち切って接続を維持 |
| `WSL request limit reached` | クライアントの未完了要求が128件。新しい要求だけを拒否 |
| `WSL request queue is full or ID is reused` | ヘルパーの要求数128件、またはIDの重複。ヘルパーが終了し接続を失う |
| `Too many Git arguments` | Git引数が64個超。Git操作だけを拒否 |
| `File cannot be saved without a valid version and a size below 16 MiB` | WSL保存で版情報なし、または16 MiB超過。通信前に保存を拒否 |
| `Invalid Linux helper size` | 配布ヘルパーが空、または32 MiB超過。接続を開始できない |
| `Invalid or oversized dirty-buffer exclusions` | 検索時に除外する未保存ファイルのパスが無効、または一覧が4 MiB超過。検索だけを拒否 |
| `Too many unsaved file exclusions` | ヘルパー側で検索除外データの4 MiB超過を検出。検索操作だけが失敗 |
| `Invalid content search query` | 空・改行・NUL・不正UTF-8、2,048 UTF-8バイト超過、または512 UTF-16単位超過。検索操作だけが失敗 |
| `Invalid WSL search match` | 検索結果のパスが4,096バイト超、抜粋が512バイト超、位置・文字コードが不正など。受信異常として接続全体を停止 |
| `Invalid WSL protocol data` | 制御64 KiB・データチャンク256 KiB・フレーム1 MiBの長さ検証、数値範囲、JSONなどが不正。通信中なら接続を失う |
| `Invalid project ID` | プロジェクトIDが空または128バイト超。ヘルパーが終了し接続を失う |
| `Terminal token is too long` | ターミナル制御用Unixソケット名がOSの格納長を超過。ターミナル操作の失敗 |
| `WSL helper stopped accepting input` / `WSL helper input timed out` | 送信待ち1 MiB以内に収まるまで待っても進まない。通常10秒で接続を停止 |
| `WSL upload timed out. Verify the file before retrying.` / `WSL upload timed out` | アップロードが要求の期限内に進まない。接続を停止 |
| `WSL operation timed out. Reconnect and verify the file before retrying a save or rename.` | 軽い操作10秒、Git20秒、一覧・検出60秒、またはキャンセル応答2秒を超過。接続を停止 |
| `WSL content search timed out; narrow the search and retry.` | 全文検索10分を超過。検索をキャンセルし、2秒以内に完了すれば接続を維持 |
| `Git operation timed out` | ヘルパー内のGit処理が15秒超過。Gitの子プロセス群を終了し、Git操作だけが失敗 |
| `Some WSL changes cannot be watched automatically. Use F5 to refresh.` | inotify監視8,192件、監視追加失敗、`/mnt/`配下で監視の制限を表示。監視できる範囲は更新を継続し、通知のない変更はF5で反映 |
| エラー文字列なし：一覧の打ち切り | 単一フォルダー一覧1万件、Git検出5万ディレクトリ・20万エントリー、表示256リポジトリ・合計5万変更件数など。切断せず制限を表示 |

この表はWSLの容量・件数・時間の制限を対象にしています。権限不足、外部変更、保存形式、接続設定などの一般エラーは [WSL対応](wsl-support.md) を参照してください。

## 検証

Linux上で、同じJSONフレームの生成・デコードと検索用データへの変換を使って比較しました。長いプロジェクトパスと短い相対パスを持つ20万件の入力です。旧方式の再現では受信JSONの64 MiB制限も再現しています。

| 方式 | 処理件数 | JSON合計 | ピークRSS | 受信上限 |
| --- | ---: | ---: | ---: | --- |
| 全項目の辞書を保持する旧方式の再現 | 122,197 | 64 MiB到達 | 約180 MiB | 超過 |
| 受信直後に検索用データへ変換 | 200,000 | 約104.9 MiB | 約36.3 MiB | 超過せず |

これは一覧処理を切り出したLinuxでの計測であり、WindowsのPoppy.exe全体の実測ではありません。利用者が覚えていたエラーと同じ受信上限を再現していますが、実際のフォルダーでの超過時の件数・バイト数は未取得です。

検索用一覧のメモリ・件数境界、上限時の既存項目の保持と検索、Unicodeとバックスラッシュの保持を単体テストで検証済みです。ヘルパー結合テストでは4 MiB超のGit出力を発生させ、その後のファイル読み込みが成功することも確認しました。Linux用の全17テストとWindows向けクロスビルドが成功しています。WindowsのGUI・実際のWSL接続・Poppy.exe全体のメモリ計測は未実施です。

Windowsでの再確認には、更新した`Poppy.exe`と同じビルドの`wsl/poppy-wsl-helper`を使い、問題のフォルダーを開きます。必要なら既存の [measure-memory.ps1](../scripts/measure-memory.ps1) でPoppy.exeのメモリを記録できます。

```powershell
$poppy = Start-Process .\out\docker\Poppy.exe -PassThru
.\scripts\measure-memory.ps1 -ProcessId $poppy.Id -Samples 60 -CsvPath poppy-wsl-memory.csv
```
