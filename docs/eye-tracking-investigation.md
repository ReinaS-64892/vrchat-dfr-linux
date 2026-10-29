# 標準 OpenXR による視線取得の実現性

調査日: 2026年10月28日。

以下は実装前の調査記録。現在は別プロセスの `openxr-gaze-bridge` と新しい
DXVK reader を実装済み。起動手順と現行仕様は [README](../README.md) と
[architecture](architecture.md) を参照。xrizer の変更や専用出力追加は行っていない。
既存セッションを用いる案は今回の実装対象外で、DXVK から既存 OpenVR interface を
利用できる経路も未確認。「共有データにする」という xrizer 独自出力案は採用しない。

**WiVRn の独自共有メモリ出力を、別プロセスの OpenXR headless 取得プログラムに置き換える方針が、最初の実装として有力。** 長期的には xrizer の既存 OpenXR セッションへ視線取得を統合する方が、描画時刻と投影を合わせやすい。DXVK から同一プロセスで新しい OpenXR instance を作る案には loader の制約がある。

現時点で確認できたのはソース上の実現性、DXVK のビルド環境、手元ランタイムの拡張公開まで。実機での視線取得、VRChat との同時動作、遅延は未検証。今回追加した診断ツールは視線取得の検証用であり、DXVK への供給プログラムではない。

## 調査対象

| 対象 | リビジョンと状態 |
| --- | --- |
| このリポジトリの DXVK パッチ | `patches/dxvk-gaze-vrs.patch` |
| DXVK の作業コピー | `.work/dxvk`、`d30be2baea02a67b9502bffda0ec8b0d915bb3d2` に既存パッチ適用 |
| ローカル xrizer | `/home/reina/xrizer`、`06fcf8f13fc8c4c5dbc3cdea0c08b6bd2e3a0a2b`、調査時 clean |
| oscavmgr | `.work/oscavmgr`、`512e4916f935c76cb8753ae404e69f1518c2abdd` |
| ローカル WiVRn | `/home/reina/WiVRn`、`a353156282901d81605b003a8f5237e8880f6774`、既存の変更あり。編集していない |
| ローカル WiVRn の展開済み Monado | `_deps/monado-src`、`79c1701d532c2898fd8a3f9b48c350ae3e62019e` |
| このリポジトリが指定する WiVRn | `bbc6e4cc36c355fa6180980abd231673dc15115d`。ローカル実行環境とは異なる |

ローカル WiVRn の `monado-rev` と展開済み Monado の HEAD も異なる。以下の Monado の細部は展開済みソースの調査結果で、ロードされるバイナリとの完全一致を保証しない。

## 現在の供給形式

WiVRn パッチは `server/compositor/foveation.cpp` で、合成された gaze と左右 FOV、眼の位置、仮定した収束距離から左右の中心 UV を計算する。元から左右それぞれの視線をそのまま渡しているわけではない。

DXVK は Windows DLL であり、Wine 経由で `Z:\dev\shm\wivrn_gaze` を `CreateFileMappingA` / `MapViewOfFile` する。`DXVK_GAZE_FILE` で別ファイルを指定できるので、新しい取得プログラムを独立させ、VRS 処理を再利用できる。

共有データには既に不一致がある。WiVRn の構造体は 28 bytes、DXVK の `GazeShm` は `flipX[2]` / `flipY[2]` を含む 44 bytes。WiVRn はファイルを 64 bytes にするため初回は末尾がゼロになるが、この偶然に依存せず、新しい writer は 44 bytes の内容と予約領域を明示的に初期化する必要がある。

既存形式は magic `0x455a4147`、偶数なら安定した sequence、`cx[2]`、`cy[2]`、`valid`、flip の順。timestamp、ABI version、writer の生存確認がないため、writer の異常終了後も最後の有効視線を読み続ける。製品化する際は DXVK reader に sequence 更新の timeout と無効化を加える。通常終了で `valid=0` を書くだけでは異常終了を扱えない。メモリ順序も両側で見直し、既存の `volatile` 読みを堅牢な同期の保証と見なさない。

## ルート1 既存コンテキストへの統合

### DXVK が OpenXR ハンドルを探して読む案

OpenXR に既存の `XrInstance` / `XrSession` を列挙して横取りする標準 API はない。OpenVR の既存 interface を取得することと、xrizer 内部の OpenXR handle を受け取ることは異なる。

ハンドルだけ渡されても足りない。`XR_EXT_eye_gaze_interaction` は instance 作成時に有効化し、pose action を生成して binding を提案し、session に action set を attach してから sync / locate する。action set の attach はセッションにつき一度なので、ゲームの attach 完了後に DXVK が後付けする方法は取れない。[拡張の仕様](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/ext/ext_eye_gaze_interaction.adoc)、[attach の仕様](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrAttachSessionActionSets.html)。

実装するなら次のいずれかが必要。

- **xrizer を拡張する。** instance の拡張選択、manifest / legacy の action attach、sync、session 再生成のライフサイクルに gaze を組み込む。描画に使う時刻と左右 projection を再利用し、結果を小さな共有データにするか OpenVR API として返す。
- **ネイティブ側の OpenXR API layer を作る。** instance 作成時の拡張、action attach / sync、フレーム時刻を intercept して視線を取得する。WiVRn と xrizer のソース改変は避けられるが、複数の action set と session の破棄・再作成を扱うため、最初の実装としては複雑。

DXVK の描画スレッドで独自に `xrSyncActions` や `xrPollEvent` を呼ぶと、xrizer の入力同期やイベント処理と競合する。取得側がライフサイクルを所有し、DXVK は最新の完成したサンプルだけ読む構成がよい。

### ローカル xrizer の実装状況

OpenVR 側には、目的にかなり近い API が存在する。[Valve のヘッダ](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h) にある `IVRSystem::GetEyeTrackedFoveationCenter` は左右 NDC を直接返す。別投影行列用の `GetEyeTrackedFoveationCenterForProjection` もある。`IVRInput` には `GetEyeTrackingDataRelativeToNow` / `GetEyeTrackingDataForNextFrame` がある。

ただし、ローカル xrizer は次の状態。

| 場所 | 状態 |
| --- | --- |
| `src/openxr_data.rs` の extension 選択 | `ext_eye_gaze_interaction` を有効化していない |
| `src/system.rs:464` | `GetEyeTrackedFoveationCenter` は warning を出して `false` |
| `src/system.rs:472` | projection 指定版も `false` |
| `src/input.rs:1041` と `:1052` | 視線データ取得は `NoData` |
| `src/input/action_manifest.rs:172` | manifest 側の action attach があり、ここへの統合が必要 |
| `src/input/legacy.rs:94` | legacy 側にも独立した attach 経路がある |

**現在の xrizer のまま DXVK から OpenVR 視線 API を呼んでも取得できない。** まず xrizer の取得・投影を実装する必要がある。最初に foveation center API を実装すると、DXVK の用途に合い、combined ray から得られない実測 fixation point を捏造せずに済む。

DXVK は PE DLL、xrizer は Linux の runtime なので、OpenVR 経由で呼ぶ場合は Proton の vrclient bridge が該当バージョンの interface / method を転送できることも確認が必要。ヘッダにメソッドが存在するだけでは、この経路の動作確認にはならない。今回 Proton 側の対応までは検証していない。

## ルート2 新しい OpenXR 接続

### oscavmgr の実例

[oscavmgr の取得実装](https://github.com/galister/oscavmgr/blob/512e4916f935c76cb8753ae404e69f1518c2abdd/src/core/ext_tracking/openxr.rs) は、新規 instance に `XR_MND_headless`、`XR_EXT_eye_gaze_interaction`、Linux の時刻変換拡張を有効化する。graphics binding のない headless session を作り、`/user/eyes_ext/input/gaze_ext/pose` を action に結び、`VIEW` 空間に対して locate する。

これは視線取得の構成として直接参考になる。ただし oscavmgr はアバターの表情制御用で、FOV による左右 UV 変換や DXVK への出力はない。予測時刻の選び方も VRS 向けには独自に設計する。OSC の送受信を経由する必要はない。

WiVRn 通常版には eye tracker の公開経路が既にある。[このリポジトリの pin の driver](https://github.com/WiVRn/WiVRn/blob/bbc6e4cc36c355fa6180980abd231673dc15115d/server/driver/wivrn_session.cpp) も HMD の `eye_gaze` を見て tracker を登録している。共有メモリ export パッチを使わずに標準 API で読むことはソース上可能。ただし拡張名の公開と、接続中 HMD の対応、実データの有効性は別々に確認する。

### 同一プロセスと別プロセス

Khronos loader の `LoaderXrCreateInstance` は、既に生きた instance がある場合、2個目を `XR_ERROR_LIMIT_REACHED` で拒否する。[loader の実装](https://github.com/KhronosGroup/OpenXR-SDK/blob/main/src/loader/loader_core.cpp)。xrizer が使う同じ native loader に新規接続する方式は、そのままでは成立しない。

既存 instance に2個目の session を作る案も、ローカル Monado IPC server の `ipc_handle_session_create` が既に session のある client を拒否するため、単純な解決にならない。Windows loader と native loader を別に使うなどの構成は、Proton の bridge / loader 実装への依存が残り、ここでは成立を確認していない。

**別の Linux プロセスなら loader の instance を分離できる。** Monado の IPC も client ごとに session を持つ。ローカルの `oxr_session_begin` は headless session を compositor に依存せず FOCUSED に遷移させる。`xrWaitFrame` は不要で、呼ばずに時刻変換と `xrLocateSpace` を使えば描画用アプリの frame loop に入らずに済む。[headless の仕様](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/mnd/mnd_headless.adoc)。

この構造から WiVRn / Monado では実現性が高いと判断する。ただし headless という拡張名だけで、すべての runtime の同時アプリ動作を保証はできない。VRChat を起動した状態で、両方が動作し続けることを実機で確認する。

## 左右 UV と遅延の設計

### 単一視線から左右 UV への変換

標準 gaze 拡張が提供するのは単一 pose であり、左右それぞれの眼球姿勢や実測の注視距離を得られるわけではない。左右に同じ UV をコピーすると、非対称 FOV や眼の回転を無視してしまう。

取得する gaze と左右 `XrView` を同じ基準空間、同じ時刻で評価する。`xrLocateViews` で各眼の pose / FOV を取得し、方向を眼の空間に変換する。距離を無限遠と仮定した場合、眼空間の方向 `d` から次のように投影できる。

```text
tx = d.x / -d.z
ty = d.y / -d.z
u  = (tx - tan(angleLeft)) / (tan(angleRight) - tan(angleLeft))
v  = (tan(angleUp) - ty) / (tan(angleUp) - tan(angleDown))
```

`v` は上端0。前方に向いた有限の方向であること、FOV の分母、pose の有効性を検査する。有限距離を使う場合は gaze origin と方向から仮の注視点を作り、各眼の平行移動も含めて変換する。既存 WiVRn の通常値は収束距離1mの仮定なので、これを再現する案と無限遠の案を比較する。どちらも実測の注視距離ではない。

headless session で `xrLocateViews` が必要な情報を返すことは実機の検証項目。ローカル Monado の `oxr_session_locate_views` には HMD の FOV / pose 取得経路がある。アプリが独自 projection や texture bounds を使う場合、runtime の FOV だけでは描画位置と一致しないことがある。VRChat の実際の投影・左右レイアウトに合わせて確認する。

既存 `DXVK_GAZE_FLIP_Y`、左右交換、array / side by side 判定、pass format の選別は残す。WiVRn のストリーム用 foveation と、DXVK の描画前 VRS は別段階なので、共有メモリ出力をなくしても WiVRn 本来の foveation 機能をなくす必要はない。

### 遅延

共有メモリによる別プロセス化は、必ずしも描画を1フレーム待たせない。最新値を非同期で読み、描画側で通信待ちをしない設計にする。たとえば250Hz取得ならポーリング待ちだけで平均約2ms、最大約4msという計算になるが、これは実測値ではなく、スケジューリング、HMD更新周期、runtime IPC などを含まない。

既存セッション統合の利点は、このポーリング待ちを減らし、描画の `predictedDisplayTime` と正確な投影を利用できること。ただし実際の眼球情報は runtime が予測できない場合もある。`XrEyeGazeSampleTimeEXT` が0なら時刻不明として扱い、取得時刻との差をそのままセンサーの end to end 遅延と呼ばない。

新しい共有 ABI を設計する場合は、左右 UV、validity、sequence に加えて version / size、取得時刻、取得できればサンプル時刻を持たせる。DXVK での stale 判定は reader 自身の steady clock と sequence の変化を使えば、Wine / Linux の時計の対応を仮定せずに実装できる。

## 実装の順序

1. `openxr-gaze-probe` を HMD 接続中と VRChat 実行中に動かし、headless の状態、action active、tracked / valid を確認する。
2. 小さな native writer を実装し、左右 FOV / pose と gaze から UV を計算する。専用の共有ファイルを使い、既存 WiVRn writer と競合させない。
3. DXVK の入力に timestamp / sequence 停止の検出を追加し、切断・取得不能時は VRS を解除する。再接続と writer 再起動も扱う。
4. gaze dot で上下左右、頭の回転、左右端、HMD脱着、writer強制終了を確認し、既存方式と精度・フレーム時間・sample age を比較する。
5. この経路で投影と有効性の条件を固めた後、必要なら xrizer の `GetEyeTrackedFoveationCenter` を実装し、同じ DXVK reader / VRS backend に供給する。

標準化する対象はまず「runtime からの取得 API」。共有メモリによる DXVK への受け渡しまで OpenXR が標準化するわけではない。

## 開発環境と診断方法

Arch の依存パッケージは `base-devel git meson ninja mingw-w64-gcc glslang openxr`。不足していた Meson と MinGW はユーザーが導入した。

```bash
bash scripts/setup-dev.sh
bash scripts/build-dxvk.sh
bash scripts/build-openxr-probe.sh

# セッションを作らず、選択中 runtime の必要拡張を確認
./build/openxr-gaze-probe

# WiVRn と HMD を接続してから。VRChat なし / ありの両方で実行
./build/openxr-gaze-probe --seconds 15
```

`setup-dev.sh` は DXVK の pin を検査し、submodule を用意し、パッチを未適用の場合だけ適用する。既存の異なる HEAD は切り替えずに終了する。`build-dxvk.sh` は incremental build を使い、並列数は `DXVK_BUILD_JOBS` で変更できる。

probe は native Linux 実行ファイル。`XR_RUNTIME_JSON` で WiVRn runtime の manifest を指定できる。デフォルトでは手元の `~/.config/openxr/1/active_runtime.json` が WiVRn の開発用 manifest を指していた。Proton / Steam コンテナ内で DXVK と共有ファイルが同じ inode を見ているかは別途確認し、必要ならコンテナから見える共有ディレクトリに置く。

検証済み: pin への DXVK パッチ適用、64bit の `d3d11.dll` / `dxgi.dll` のビルド、増分ビルドと `dist/dxvk/` へのコピー、セットアップの再実行、probe のビルドと引数処理、shell 構文と diff の検査。生成した DLL は Proton へインストールしていない。

probe の出力は session state、sync の戻り値、action active、space flags、head に対する方向、利用可能な場合の sample age。`--seconds` はサンプリングループの長さであり、runtime API がブロックする場合の hard timeout ではない。`valid=1` が一度以上あれば終了コード0、それ以外は1、不正な引数は2。

この環境では拡張3個の列挙は成功。セッション作成は `/run/user/1000/wivrn/comp_ipc` が存在せず `XR_ERROR_RUNTIME_UNAVAILABLE` で終了した。WiVRn の起動や HMD 接続は自動では変更していない。
