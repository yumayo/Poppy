# Poppyとai-containerの接続

WSLにDocker Engineをインストールし、普段どおり`aicontainer`で起動する構成に対応します。PoppyのWSLターミナルからなら、**`.aicontainer`への通知用設定や、Poppy通知専用の`allow-ip`は不要です。** CLI別hookの導入は引き続き必要です。

通知は`hook → マウントされたUnixソケット → WSLヘルパー → 匿名パイプ → Poppy`を通ります。Poppyは通知用のTCP/HTTPポートを開きません。通知先のセッションIDはPoppy側で保持します。対話ターミナルの標準入出力とファイル操作の接続には通知を混ぜません。wslc / WSL ContainersはPoppyのサポート対象外です。

## 起動と更新

1. PoppyでWSLプロジェクトを開き、ターミナルの「＋」からWSLシェルを開きます。
2. WSLホストの`.bash_ai_container`を更新し、利用しているコピーを`source`で読み込みます。
3. 下記のhookをAIコンテナ内へ導入します。HTTP版のhookを導入済みの場合も送信スクリプトを一度更新してください。
4. PoppyのWSLターミナルで、普段どおり起動します。

```sh
aicontainer codex
# または
aicontainer claude
```

PoppyはターミナルごとのUnixソケットをシェル起動前に開き、そのパスを`AICONTAINER_EVENT_SOCKET`へ自動設定します。更新した`aicontainer`はこのソケットだけをコンテナの`/tmp/aicontainer-events.sock`へマウントし、hookへ引き継ぎます。`.aicontainer`への追加、起動プロファイル、ai-containerリポジトリのパス指定は不要です。この変更だけならDockerイメージの再ビルドも不要です。

Poppy本体と同じ場所にリリースの`wsl/`フォルダーを配置してください。Poppy更新後はWSLターミナルも開き直します。ai-container側をコピーして利用している場合は、そのコピーも更新してください。Dockerデーモンは、起動元のWSLと同じソケットの実体を参照できる必要があります。リモートDockerへの転送は対象外です。

WSLプロジェクトでは「＋」の右クリックにある`New Codex (ai-container)` / `New Claude Code (ai-container)`でも起動できます。WSLユーザーのBash起動設定から`aicontainer`を読み込み、通常と同じコンテナを起動します。Windowsプロジェクトでは通常のシェルだけを提供します。

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

## 旧通知設定の削除

HTTP通知とWindows上のCLI向け通知は廃止しました。`.aicontainer`に残っている旧通知用の環境変数と、Poppy通知のためだけに追加した`allow-ip`は削除できます。

更新後はPoppyとWSLターミナルを開き直してください。起動中の旧Poppyの待受は、旧プロセスを終了するまで残ります。

ソケットが設定されていない環境では通知せず、AIの処理は継続します。ソケットへの送信失敗でもAIの処理を停止しません。HTTPへの切り替えはありません。

## 所有範囲と上限

ソケットはWSLのランダムな専用ディレクトリ（0700）へ作成し、Dockerにはソケット1個だけを渡します。ソケットの権限は0666で、WSLとコンテナのUIDが異なる場合も接続できます。ホスト上の他ユーザーからは親ディレクトリで保護します。AIコンテナへDocker権限は追加しません。

接続は最大32件、受信期限は2秒、1通知は16 KiB、本文は8 KiBです。匿名パイプへ送る待ち行列は最大1,024件・1 MiBで、上限時は`BUSY`を返します。`OK`はリレーへの受け付けを表し、Poppyでの最終反映の確認ではありません。Poppy側の配送待ちも最大1,024件です。

ターミナルの終了やPoppyとのパイプ切断でリレーを終了し、ソケットと専用ディレクトリを削除します。ターミナルごとに経路が独立するため、別のターミナルの通知は継続します。同じターミナル内で独立したAIを並列実行すると通知先を共有するため、AIごとにターミナルを分けてください。Dockerデーモン上のコンテナの停止・後始末は通常の`aicontainer`の管理範囲です。

## 検証

- `python3 tests/hook_scripts_tests.py`: Unixソケット通知、ソケット未設定時の動作、hookの導入・解除。
- `python3 tests/hook_relay_tests.py`: ビルド済みの実WSLヘルパーと導入済みhookをUnixソケット・匿名パイプで接続。別のビルドを使う場合は`POPPY_TEST_WSL_HELPER=/path/to/poppy-wsl-helper`を指定。
- ai-containerの`python3 -m unittest discover -s test -p test_aicontainer_dns.py`: Dockerモックで通常起動時のソケット自動マウントとパスの引用を検証。

Windows上の`wsl.exe`、WSLENVによる引き継ぎ、実際のDockerマウントとCLIのhook発火は実機確認が必要です。
