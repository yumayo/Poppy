# Poppyとai-containerの接続

通知の受信口は既定で `0.0.0.0:5010` です。通知のためにPoppy起動時の環境変数を設定する必要はありません。コンテナからの通知先は `.aicontainer` に設定します。

```ini
allow-ip=172.17.176.1
env=POPPY_EVENTS_URL=http://172.17.176.1:5010
env=POPPY_SESSION_ID
```

`172.17.176.1` は例です。コンテナから到達できるWindowsのIPv4に置き換えてください。`POPPY_SESSION_ID` はPoppyがターミナルごとに発行するため、値を固定せず名前だけを指定します。設定変更後はAIコンテナを作り直してください。CLI別hookの導入も必要です（後述）。

WindowsファイアウォールでPoppyへの受信を許可し、コンテナからWindowsへの経路があることを確認してください。`allow-ip` は通信の許可であり、経路は作りません。コンテナ内のlocalhostはWindowsを指しません。

必要な場合だけ `POPPY_HOOK_BIND` と `POPPY_HOOK_PORT` で待受を変更できます。ポートを変更する場合は `.aicontainer` の通知先も合わせてください。`POPPY_HOOK_HOST` はWindows上の通常シェルへ渡すURLの宛先を変更する設定で、コンテナの通知先URLは `.aicontainer` の値を使います。

## AIコンテナの起動

Poppyのターミナルの「＋」は通常のシェルを追加します。「＋」の右クリックで、通常のシェル、Codex、Claude Codeを選べます。AIを起動する場合は、Windows側の環境変数 `POPPY_AI_CONTAINER_DIR` に、利用しているai-containerのチェックアウトを指定してからPoppyを起動してください。

```powershell
$env:POPPY_AI_CONTAINER_DIR = 'C:\work\ai-container'
.\Poppy.exe 'C:\work\feature-a'
```

Poppy本体と同じ場所の `ai/` フォルダーを配布してください。アダプターは既存の `wsl-containers/wsl-containers.ps1` の関数と `LaunchProfileFile` を使います。AI用の起動プロファイルは、ワークスペースの `.poppy-runtime/<UUID>/launch.json` に作成し、ターミナルの通知IDだけを渡します。通知先URLはコンテナの設定を引き継ぎます。各プロジェクトのGit除外設定に `.poppy-runtime/` を追加してください。共有の `.aicontainer`、CLI設定、認証情報は起動ごとに書き換えません。

## hookの導入・解除

通知の受信・状態の保持・プロジェクトごとの集約はPoppyが行います。配布するCLI別hook設定と送信スクリプトもPoppy専用です。各CLIは `hooks/poppy/poppy_status.py` を呼び出し、PoppyのHTTP受信口へ状態を通知します。

`ai/scripts` と `ai/examples` を、AIを実行するLinux環境から参照できる場所に配置し、**AIコンテナ内で**利用するCLIの設定先に導入します。保存先はユーザーのai-containerの永続設定に合わせてください。

```sh
python3 /path/to/ai/scripts/install-codex-hooks.py
python3 /path/to/ai/scripts/install-claude-hooks.py
# 設定先を指定する場合
python3 /path/to/ai/scripts/install-codex-hooks.py --codex-dir /path/to/.codex
python3 /path/to/ai/scripts/install-claude-hooks.py --claude-dir /path/to/.claude
```

既存の設定を保持し、変更時にはバックアップを作成します。再実行しても同じhookを重複登録しません。CLIを再起動し、各CLIの `/hooks` で設定を確認してください。hookに対応したCodex・Claude Codeのバージョンが必要です。実CLIでの発火確認はWindows / WSL / Dockerを含む利用環境で実施してください。

```sh
python3 /path/to/ai/scripts/uninstall-hooks.py codex
python3 /path/to/ai/scripts/uninstall-hooks.py claude
# 必要なら --config-dir で対象を指定する。
```

解除は `hooks/poppy/poppy_status.py` の通知コマンドだけを除去し、他のhookと設定を残します。

## 通常のシェルから起動する場合

Poppyはシェル起動ごとにWindows上のCLI向けの `POPPY_EVENTS_URL`（既定は `http://127.0.0.1:5010`）と32文字の `POPPY_SESSION_ID` を設定します。`WSLENV` の既存設定を維持して、この2変数も引き継ぎます。WSL経由で手動起動する場合も、冒頭の `.aicontainer` 設定を使ってください。コンテナでは通知先URLをWindowsのIPv4に切り替え、通知IDをPoppyのシェルから引き継ぎます。

固定の通知IDを共有設定へ書かないでください。同じターミナル内で独立したAIを並列実行すると通知が混ざるため、AIごとに別ターミナルを使ってください。

通常のPoppyシェルから、このアダプターを直接呼ぶこともできます。

```powershell
& 'C:\path\to\Poppy\ai\start-ai.ps1' -Repository $env:POPPY_AI_CONTAINER_DIR -Workspace (Get-Location).Path -Tool codex
```

## 終了と制約

このアダプターは、ホスト側の名前付きイベントを使ってターミナルの停止要求を受け取ります。既存ランチャーが渡す対象のランチャー名を直接取得し、そのランチャーだけを停止します。その後、既存の `finally` による後始末を待ち、対象のコンテナ、リレー、ネットワーク、ボリュームが残っていないことを照会して確認します。AIコンテナへDocker権限を追加する方式ではありません。

ターミナル終了時は確認を最大15秒待ちます。確認できない場合は「cleanup unconfirmed」を表示し、起動プロファイルを残します。ランチャーが対話接続前に失敗して起動UUIDを取得できなかった場合も、後始末済みと推測しません。Windowsクライアントの終了だけでDockerコンテナの終了を保証しません。アダプターを通さず起動したコンテナの終了管理は対象外です。

PoppyのHTTP通知形式は `running` / `waiting` / `interrupted` / `completed` / `none` と `activityText`、`onlyIfActive` です。質問・承認の区別、依頼ID、連番、再送・heartbeatは含みません。次の依頼開始後に遅れて届く前の依頼の完了を厳密には排除できません。無出力時間、Ctrl+C送信、一般のツール失敗からAI状態を補完しません。受信済み状態の時刻・判定元はツールチップに表示します。

## 検証

- `python3 tests/hook_scripts_tests.py`: 送信値、Claudeのバックグラウンド補正、既存設定の保持、導入の冪等性、解除を検証。導入済みのCodex・Claude Codeのhookコマンドを実行し、`POPPY_EVENTS_URL` / `POPPY_SESSION_ID` を使ったlocalhostのHTTP受信まで確認する。
- `poppy_ai_status_tests`: 状態遷移、集約、JSONと上限の検証。
- `poppy_hook_server_tests`: Windows上でHTTP受信、端末IDへの所属、遅着、遅い接続、サイズ上限を検証。
- `poppy_folder_tests`: Windows上で保持済みプロジェクトの切り替えと複数端末を検証。

Linux側のテストとWindows向けコンパイルを実施しています。Windows上のGUI・ConPTY・実CLI hook、およびこのPowerShellアダプターによる起動・停止の実動作は未検証です。

## WSLプロジェクトからの直接起動

WSLプロジェクトの「＋」の右クリックでは、同じディストリビューション・ユーザー・Linuxのcwdで、WSLにインストールしたCodex・Claude Codeを直接起動します。`POPPY_AI_CONTAINER_DIR`は使いません。既存のhook導入スクリプトをWSL側のCLI設定へ導入し、WSLから到達できるWindowsの通知先を設定してください。通知IDとURLは既存の`WSLENV`を保って引き継ぎます。

Windows用のai-containerランチャーにWSLのLinuxパスを渡す接続は未実装です。Windowsプロジェクトのai-container起動は従来の手順を利用できます。ファイル接続と通知の到達性は別に検証してください。[WSL対応](../../docs/wsl-support.md)に利用範囲と制約をまとめています。
