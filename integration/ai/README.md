# Poppyとai-containerの接続

WSLにDocker Engineをインストールし、普段どおり`aicontainer`で起動する構成に対応します。PoppyのターミナルからWSLで起動すれば、**`.aicontainer`への通知用設定や、Poppy通知専用の`allow-ip`は不要です。** CLI別hookの導入は引き続き必要です。

通知は`hook / polling → マウントされたUnixソケット → WSLヘルパー → 匿名パイプ → Poppy`を通ります。Poppyは通知用のTCP/HTTPポートを開きません。通知先のセッションIDはPoppy側で保持します。対話ターミナルの標準入出力とファイル操作の接続には通知を混ぜません。wslc / WSL ContainersはPoppyのサポート対象外です。

## 起動と更新

1. Poppyの「＋」からターミナルを開きます。PowerShellまたはGit Bashが起動した場合は`wsl`を実行します。WSLプロジェクトでは直接WSLシェルが起動します。
2. WSLホストの`.bash_ai_container`を更新し、利用しているコピーを`source`で読み込みます。
3. 下記のhookをAIコンテナ内へ導入します。HTTP版のhookを導入済みの場合も送信スクリプトを一度更新してください。
4. PoppyのWSLターミナルで、普段どおり起動します。

```sh
aicontainer codex
# または
aicontainer claude
```

PoppyはターミナルごとのUnixソケットをシェル起動前に開き、そのパスを`AICONTAINER_EVENT_SOCKET`へ自動設定します。WindowsプロジェクトのPowerShell・Git Bashでも、既定のWSL内にソケットを用意し、手入力の`wsl`へ`WSLENV`で引き継ぎます。Git Bashのパス変換でLinux側の`/tmp/poppy-events-…/events.sock`がWindowsの一時フォルダーへ書き換わらないよう、`MSYS2_ENV_CONV_EXCL`へこの変数だけを追加します。既存の除外設定は保持します。更新した`aicontainer`はこのソケットだけをコンテナの`/tmp/aicontainer-events.sock`へマウントし、hookへ引き継ぎます。`.aicontainer`への追加、起動プロファイル、ai-containerリポジトリのパス指定は不要です。この変更だけならDockerイメージの再ビルドも不要です。

PowerShell経由でも、Poppy本体と同じ場所にリリースの`wsl/`フォルダーを配置してください。Poppy更新後はWSLターミナルも開き直します。ai-container側をコピーして利用している場合は、そのコピーも更新してください。Dockerデーモンは、起動元のWSLと同じソケットの実体を参照できる必要があります。リモートDockerへの転送は対象外です。

WSLプロジェクトでは「＋」の右クリックにある`New Codex (ai-container)` / `New Claude Code (ai-container)`でも起動できます。WSLユーザーのBash起動設定から`aicontainer`を読み込み、通常と同じコンテナを起動します。WindowsプロジェクトではPowerShellから`wsl`へ入り、上記の`aicontainer`コマンドを使ってください。

PowerShell経由は、ターミナル起動時の既定WSLディストリビューション・既定Linuxユーザーが対象です。`wsl -d`や`wsl -u`で別の環境を使う場合は、対象のWSLプロジェクトを開いて直接ターミナルを起動してください。WSLや同梱ヘルパーを使えない場合もPowerShellは起動し、通知を利用できない理由をターミナルの説明へ表示します。

## hookの導入・解除

`ai/scripts`と`ai/examples`を、AIコンテナから参照できる場所に配置し、**AIコンテナ内で**利用するCLIの設定先に導入します。保存先はai-containerの永続設定に合わせてください。

```sh
python3 /path/to/ai/scripts/install-codex-hooks.py
python3 /path/to/ai/scripts/install-claude-hooks.py
# 設定先を指定する場合
python3 /path/to/ai/scripts/install-codex-hooks.py --codex-dir /path/to/.codex
python3 /path/to/ai/scripts/install-claude-hooks.py --claude-dir /path/to/.claude
```

既存設定を保持し、変更時にはバックアップを作成します。再実行しても同じhookを重複登録しません。CLIを再起動し、各CLIの`/hooks`で設定を確認してください。hookに対応したCodex・Claude Codeのバージョンが必要です。

```sh
python3 /path/to/ai/scripts/uninstall-hooks.py codex
python3 /path/to/ai/scripts/uninstall-hooks.py claude
# 必要なら --config-dir で対象を指定する。
```

解除は`hooks/poppy/poppy_status.py`の通知コマンドだけを除去し、他のhookと設定を残します。

## Codexのpolling監視

Codexでは、AIコンテナ内の監視プロセスが**0.5秒ごとにローカル履歴を読み取り**、実行中・完了・中断を反映します。状態が変わったときと2秒ごとに状態を再送するため、開始・終了のhookや通知を取りこぼしても表示を修復できます。モデルへの問い合わせや外部通信は行いません。Poppy.exeの通知形式は変更していないため、スクリプトの更新だけで利用できます。

`install-codex-hooks.py`を再実行すると、送信スクリプトと`poppy_codex_poll.py`を更新します。監視は最初に届いたCodexのhookで一度だけ起動します。開始hookが欠けても、後続のツールhookなどから起動できます。**起動中のCodexへhookなしで接続する場合**は、そのCodexと同じAIコンテナ・ターミナルで次を実行します。

```sh
python3 "${CODEX_HOME:-$HOME/.codex}/hooks/poppy/poppy_codex_poll.py"
# 対象のCodexプロセスを明示する場合
python3 "${CODEX_HOME:-$HOME/.codex}/hooks/poppy/poppy_codex_poll.py" --pid 123
```

CLIが開いている会話ファイルで対象を特定し、別ターミナルや子エージェントの履歴を選びません。新しいCodexの履歴DBを読み取り専用で参照し、DBのない形式では会話JSONLの追記を読みます。入力・承認待ちはhookの情報で補助します。無出力時間、コンパクト、履歴の一時的な読取失敗を理由に`none`へ変更しません。CLIの終了時に監視も終了して`none`を送ります。CLIとコンテナが同時に終了する構成に備え、`SessionEnd`のhookからも終了通知を直接送ります。

Claude Codeは引き続きhookで通知します。Codexの履歴DBとJSONLはCLI内部の保存形式なので、今後の形式変更には追従が必要です。現在導入されているCodex 0.166.0の履歴形式と、旧形式のJSONLをテスト対象にしています。

監視だけを終了するには、上記スクリプトへ`--stop`を指定します。hookを残すと次のhookで再起動するため、連携を解除する場合は`uninstall-hooks.py codex`を使ってください。解除時は起動中の監視も停止します。監視情報と二重起動を防ぐロックは、コンテナの一時ディレクトリへ保存します。

## 旧通知設定の削除

HTTP通知とWindows上のCLI向け通知は廃止しました。`.aicontainer`に残っている旧通知用の環境変数と、Poppy通知のためだけに追加した`allow-ip`は削除できます。

更新後はPoppyとWSLターミナルを開き直してください。起動中の旧Poppyの待受は、旧プロセスを終了するまで残ります。

ソケットが設定されていない環境では通知せず、AIの処理は継続します。ソケットへの送信失敗でもAIの処理を停止しません。HTTPへの切り替えはありません。

## 所有範囲と上限

ソケットはWSLのランダムな専用ディレクトリ（0700）へ作成し、Dockerにはソケット1個だけを渡します。ソケットの権限は0666で、WSLとコンテナのUIDが異なる場合も接続できます。ホスト上の他ユーザーからは親ディレクトリで保護します。AIコンテナへDocker権限は追加しません。

接続は最大32件、受信期限は2秒、1通知は16 KiB、本文は8 KiBです。匿名パイプへ送る待ち行列は最大1,024件・1 MiBで、上限時は`BUSY`を返します。`OK`はリレーへの受け付けを表し、Poppyでの最終反映の確認ではありません。Poppy側の配送待ちも最大1,024件です。

ターミナルの終了やPoppyとのパイプ切断で、リレーは新しい起動用のソケットを削除し、同じソケットをマウントしたAIコンテナを停止・削除します。Dockerのコンテナ名やCLIの名前では判定せず、マウント元がそのターミナル専用ソケットのパスと一致するものだけが対象です。Docker操作はWSLホスト側で行い、AIコンテナへのDocker権限の追加は不要です。

次に`AICONTAINER_EVENT_SOCKET`を引き継いだWSLプロセスとその子孫を終了し、ロックを解放するまで待ちます。PowerShell／Git Bashから手入力した`wsl`も対象です。WSLでは子からSIGTERMを送り、700 msで終了しないプロセスにはSIGKILLを送り、終了確認を続けます。最後にWindows側のジョブ内の全プロセスの終了を待ちます。通常の終了では、この後始末が終わるまでPoppy.exeは終了しません。Dockerがインストールされていない通常のシェルではコンテナ操作を省略します。Dockerがある環境で照会・停止を確認できない場合は再試行し、終了待ちを続けます。

ターミナルごとに経路が独立するため、別のターミナルは終了しません。同じターミナル内で起動したAIは同じ終了対象になります。以前のPoppyから残ったコンテナや、通知ソケットを引き継がず起動した別の環境は識別できません。更新時はPoppy.exeと同梱の`wsl/`フォルダーを一緒に置き換え、ターミナルを開き直してください。

## 検証

- `python3 tests/hook_scripts_tests.py`: Unixソケット通知、ソケット未設定時の動作、hookの導入・解除。
- `python3 tests/codex_poll_tests.py`: 開始・完了・中断・終了hookを省略した状態の復旧、再送、入力待ち、会話切り替え、二重起動防止、コンパクトと途中のJSONL、履歴DBの読み取り。
- `python3 tests/hook_relay_tests.py`: ビルド済みの実WSLヘルパーと導入済みhookをUnixソケット・匿名パイプで接続。PowerShell向けの単独ヘルパー配置・再利用、子孫プロセスのロック解放、別ターミナルの保持、Dockerモックによる所有判定・停止完了待ち・照会失敗時の再試行も検証。別のビルドを使う場合は`POPPY_TEST_WSL_HELPER=/path/to/poppy-wsl-helper`を指定。
- Windowsの`poppy_render_tests`: Git for Windowsがある場合、Git BashからWindowsプロセスへ通知ソケットのLinuxパスがそのまま渡り、既存のパス変換除外も保持されることを検証。
- ai-containerの`python3 -m unittest discover -s test -p test_aicontainer_dns.py`: Dockerモックで通常起動時のソケット自動マウントとパスの引用を検証。

Windows上の`wsl.exe`、WSLENVによる引き継ぎ、実際のDockerマウントとCLIのhook発火は実機確認が必要です。
