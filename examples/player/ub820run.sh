#!/bin/busybox sh
# ub820run.subs.sh — ub820run.sh + text subtitles drawn by subovl on the UI plane (LOSD0). Otherwise identical.
# Usage: ub820run.subs.sh <file> [audio_track_index|none] [start_seconds] [subtitle_track|none|auto|/path/file.srt]
#   arg 4 (default none = same behaviour as ub820run.sh):
#     N       subtitle stream N of the file (ffmpeg 0:s:N, counting ALL subtitle streams; list in /tmp/ub820run.subtracks)
#             text codecs (subrip/srt/ass/ssa/mov_text/webvtt/text) are extracted to /tmp/ub820.srt in the background
#             with the static ffmpeg (cached per file+track); bitmap codecs (PGS, DVD, DVB) are not supported yet.
#     auto    a sidecar <file>.srt / .vtt next to the media if present, else the first text subtitle stream
#     PATH    an external .srt / .vtt file
#   Subtitles are drawn on the 4K route too (default since 2026-11-28; UB820_SUBS_4K=0 turns them off there).
#   UB820_SUBOPT = extra subovl options, e.g. "--size 60 --offset -0.4". subovl logs to /tmp/subovl.log.
#   Probes with the static ffmpeg, then pipes a 188-byte MPEG-TS into ub820play:
#     video: MPEG-1/2, H.264, HEVC, VC-1 -> copied (hardware decode); anything else -> software-decoded and
#            re-encoded to MPEG-2 on the fly (fine for SD, ~real time at 720p).
#     audio: AAC / AC-3 / E-AC-3 / MP2 / MP3 -> copied; DTS, DTS-HD, TrueHD, FLAC, Vorbis, Opus, PCM ... -> AC-3 5.1 640k
#            (vcavsrv's codec table cannot select DTS/TrueHD; see findings/vcav-audio.md).
#     routing: --4k (4KOTT use case, home UI torn down, homerestore after) for every file; UB820_ROUTE=hd = DIVX route.
# Env: UB820_BIN (dir with ffmpeg/ub820play, default /tmp), UB820_DRY=1 (print the pipeline only).
BB=/bin/busybox; BIN=${UB820_BIN:-/tmp}; FF=$BIN/ffmpeg; PL=$BIN/ub820play
FFX=$FF; [ -n "${UB820_NICE:-}" ] && FFX="$BB nice -n $UB820_NICE $FF"   # the streaming ffmpeg(s)
export LD_LIBRARY_PATH=/usr_root/lib:/lib:/usr/lib:/usr/X11R6/lib:/usr/dtvrec/lib:/usr/local/lib
F="$1"; AT="${2:-0}"; SS="${3:-}"; ST="${4:-none}"; STARG="$ST"
[ -r "$F" ] || { echo "usage: $0 <file> [audio_track|none] [start_s] [sub_track|none|auto|file.srt]" >&2; exit 1; }
INFO=$($FF -hide_banner -i "$F" 2>&1)
VL=$(echo "$INFO" | $BB grep -m1 'Stream #.*Video:')
[ -n "$VL" ] || { echo "no video stream" >&2; exit 2; }
VC=$(echo "$VL" | $BB sed -n 's/.*Video: \([a-z0-9_]*\).*/\1/p')
RES=$(echo "$VL" | $BB grep -oE '[0-9]{2,5}x[0-9]{2,5}' | $BB head -1); W=${RES%x*}; H=${RES#*x}
FPS=$(echo "$VL" | $BB grep -oE '[0-9.]+ fps' | $BB head -1 | $BB cut -d' ' -f1)
case "$VC" in
  mpeg1video|mpeg2video) CODEC=mpeg2; VOPT="-c:v copy";;
  h264) CODEC=h264; VOPT="-c:v copy";;
  hevc) CODEC=hevc; VOPT="-c:v copy";;
  vc1)  CODEC=vc1;  VOPT="-c:v copy";;
  *)    CODEC=mpeg2
        case "$FPS" in 23.98|24|25|29.97|30|50|59.94|60) R="";; *) R="-r 30000/1001";; esac
        SC=""; [ "${W:-0}" -gt 1920 ] && SC="-vf scale=1920:-2"
        VOPT="-c:v mpeg2video -q:v 3 -g 15 $R $SC";;
esac
# raw elementary-stream input (no container timestamps): give ffmpeg a frame rate and synthesise PTS/DTS
RAWIN=""; if echo "$INFO" | $BB grep -qE '^Input #0, (hevc|h264|mpegvideo|mjpeg|cavsvideo),'; then
  TBR=$(echo "$VL" | $BB grep -oE '[0-9.]+ tbr' | $BB head -1 | $BB cut -d' ' -f1); [ -n "$TBR" ] && FPS=$TBR
  case "$FPS" in 23.98) FR=24000/1001;; 29.97) FR=30000/1001;; 59.94) FR=60000/1001;; "") FR=25;; *) FR=$FPS;; esac
  case "$FR" in */*) TSX="N*${FR#*/}/${FR%/*}/TB";; *) TSX="N/$FR/TB";; esac
  RAWIN="-framerate $FR"; [ "$VOPT" = "-c:v copy" ] && VOPT="-c:v copy -bsf:v setts=ts=$TSX"; fi
# VP9: hardware path rejects PLAY via raw TS (both use cases) -> software decode + MPEG-2 transcode (default branch)
# Routing: the 4KOTT use case for everything (2026-11-28: on the DIVX/HD route the home UI kept redrawing over the
# video, sound only; 1080p H.264 and 720p MPEG-2 play fine on 4KOTT). UB820_ROUTE=hd brings the old DIVX route back.
MODE=--4k; [ "${UB820_ROUTE:-}" = hd ] && MODE=--hd
# audio: map ALL audio tracks (max 4) so they can be switched live (ub820play ctl "audio <pid> <codec>");
# each goes on PID 0x1100+i, copied when vcavsrv can take it, else transcoded to AC-3. The start track is $AT.
AOPT="-an"; APL="--apid none"; DECOPT=""; AN=$(echo "$INFO" | $BB grep -c 'Stream #.*Audio:'); : > /tmp/ub820run.tracks
[ "$AN" -gt 4 ] && AN=4
[ -n "${UB820_MAXA:-}" ] && [ "$AN" -gt "$UB820_MAXA" ] && AN=$UB820_MAXA   # test knob: fewer audio tracks
if [ "$AT" != none ] && [ "$AN" -gt 0 ]; then
  [ "$AT" -ge "$AN" ] && AT=0
  AOPT=""; i=0
  while [ $i -lt $AN ]; do
    AL=$(echo "$INFO" | $BB grep 'Stream #.*Audio:' | $BB sed -n "$((i+1))p")
    C=$(echo "$AL" | $BB sed -n 's/.*Audio: \([a-z0-9_]*\).*/\1/p')
    LL=0; [ -f /tmp/ub820.lossless ] && [ -x ${UB820_LLBIN:-$BIN/vcavsrv.lossless} ] && LL=1
    case "$C" in
      dts)    if [ $LL = 1 ]; then X="copy"                 # patched vcavsrv: vcav 0x20+N -> AVD N
                # DTS-HD MA decodes as s32p (24 bit) in ffmpeg; core/HRA as fltp. AVD 0x10 = DTS-HD MA, 5 = DTS core.
                if echo "$AL" | $BB grep -qE 'DTS-HD MA|s32p'; then K=48; elif echo "$AL" | $BB grep -q 'DTS-HD HRA'; then K=46; else K=37; fi
                C2=1; else C2=0; fi;;
      truehd) if [ $LL = 1 ]; then X="copy"; K=38; C2=1; else C2=0; fi;;
      *) C2=0;;
    esac
    [ "$C2" = 1 ] && USE_LL=1
    [ "$C2" = 1 ] || case "$C" in
      aac)  K=0; X="copy";;
      ac3)  K=1; X="copy";;
      eac3) K=8; X="copy";;
      mp2|mp3) K=2; X="copy";;
      *)    K=1; CH=6; echo "$AL" | $BB grep -qE ' (mono|stereo|2 channels)' && CH=2
            X="ac3 -b:a:$i 640k -ar:a:$i 48000 -ac:a:$i $CH"
            # DTS-HD MA/HRA/X: decode only the DTS core for the AC-3 re-encode (the full XLL decode is CPU-heavy
            # and its bursts made the video stutter every ~5 s, 2026-11-28)
            [ "$C" = dts ] && [ "${UB820_DTSFULL:-0}" = 0 ] && DECOPT="-core_only 1";;
    esac
    PID=$((4352+i)); AOPT="$AOPT -map 0:a:$i -c:a:$i $X -streamid $((i+1)):$PID"
    LANG=$(echo "$AL" | $BB sed -n 's/.*Stream #0:[0-9]*\(([a-z]*)\)\{0,1\}.*/\1/p')
    printf '%s 0x%x %s %s %s\n' "$i" "$PID" "$K" "$C" "$LANG" >> /tmp/ub820run.tracks
    [ $i = "$AT" ] && { AC=$C; ACODEC=$K; APL="--apid $(printf 0x%x $PID) --acodec $K"; }
    i=$((i+1))
  done
fi
# Lossless mode: the patched vcavsrv must also REPORT the real coding type to mocserver, else mocserver outputs it as
# 2ch PCM (findings/hd-audio-output.md, P1 @0x66b2). One binary per format, picked by the starting track:
#   TrueHD -> vcavsrv.ll_thd (CT 6)   DTS-HD MA -> ll_ma (CT 8)   DTS-HD HRA -> ll_hr (CT 7)   DTS core -> ll_core (CT 4)
# 2026-11-28 console: MA -> IEC output "dts" bitstream (was "pcm 2D"); TrueHD -> "D1|ddg". AVR result: TBC by the user.
if [ "${USE_LL:-0}" = 1 ] && [ -z "${UB820_LLBIN:-}" ]; then
  case "$AC:$ACODEC" in truehd:*) LB=ll_thd;; dts:48) LB=ll_ma;; dts:46) LB=ll_hr;; dts:*) LB=ll_core;; *) LB="";; esac
  [ -n "$LB" ] && [ -x $BIN/vcavsrv.$LB ] && UB820_LLBIN=$BIN/vcavsrv.$LB && echo "ub820run: lossless: $UB820_LLBIN" >&2
fi
SSO=""; [ -n "$SS" ] && SSO="-ss $SS"
# Audio starting well before the first video frame (BHD: audio 0.000 s, video 1.364 s) made the decoder start its
# clock on the audio and freeze the picture ~1.2 s right after the start (2026-11-28). From the top, start at the
# first video packet instead. UB820_VSTART=0 turns it off.
if [ -z "$SS" ] && [ "$AT" != none ] && [ "${AN:-0}" -gt 0 ] && [ "${UB820_VSTART:-1}" != 0 ]; then
  VS=$($FF -nostdin -debug_ts -i "$F" -map 0:v:0 -map 0:a:$AT -c copy -frames:v 2 -f null - 2>&1 | $BB grep 'demuxer -> ' |
    $BB awk '/type:video/ && v==""{match($0,/pkt_pts_time:[-0-9.]+/); v=substr($0,RSTART+13,RLENGTH-13)}
             /type:audio/ && a==""{match($0,/pkt_pts_time:[-0-9.]+/); a=substr($0,RSTART+13,RLENGTH-13)}
             END{ if(v!="" && a!="" && v-a > 0.25) printf "%.3f", v }')
  [ -n "$VS" ] && { SSO="-ss $VS"; SS=$VS; echo "ub820run: audio starts before the video: starting at the first video frame (${VS}s)" >&2; }
fi
# Matroska keeps 1 ms timestamps (and the demuxer guesses DTS): 23.976p video then reaches the decoder with
# 41/42 ms jitter, seen as a stutter every few seconds on 4K HEVC (2026-11-28). A second, copy-only ffmpeg on the
# 90 kHz TS snaps PTS to the frame grid and rebuilds DTS as start + N frames. UB820_SNAP=0 turns it off.
SNAP=""; if echo "$INFO" | $BB grep -q '^Input #0, matroska' && [ "$VOPT" = "-c:v copy" ] && [ "${UB820_SNAP:-1}" != 0 ]; then
  case "$FPS" in 23.98|23.976) FS=1001/24000;; 24) FS=1/24;; 25) FS=1/25;; 29.97) FS=1001/30000;; 30) FS=1/30;;
                 50) FS=1/50;; 59.94) FS=1001/60000;; 60) FS=1/60;; *) FS="";; esac
  if [ -z "$FS" ]; then   # no usable frame rate in the header (e.g. "1k fps, 120 tbr"): measure 150 packets' PTS
    FS=$($FF -nostdin -debug_ts -i "$F" -map 0:v:0 -c copy -frames:v 150 -f null - 2>&1 | $BB grep 'demuxer -> .*type:video' |
      $BB sed -n 's/.* pkt_pts_time:\([0-9.]*\) .*/\1/p' | $BB sort -n | $BB awk '{p[NR]=$1} END{ if(NR<60) exit;
      d=(p[NR-10]-p[11])/(NR-21); n=split("1001/24000 1/24 1/25 1001/30000 1/30 1/50 1001/60000 1/60",c," ");
      be=1; for(i=1;i<=n;i++){ split(c[i],q,"/"); f=q[1]/q[2]; e=(d-f)/f; if(e<0)e=-e; if(e<be){be=e; bc=c[i]} } if(be<0.005) print bc }')
    [ -n "$FS" ] && echo "ub820run: matroska: measured frame period $FS s" >&2; fi
  if [ -n "$FS" ]; then
    # ffmpeg 7.1's matroska path gives B-frames DTS == PTS and its non-monotonic-DTS fix-up then mangles their PTS
    # (groups of frames 1 tick apart -> judder; 2026-11-28 capture). Rebuild DTS = first PTS + (N - 3) frames in the
    # demuxer's own (1 ms) time base: monotonic, <= PTS with 3 frames of reorder headroom; PTS left untouched.
    VOPT="-c:v copy -bsf:v 'setts=dts=round((STARTPTS*TB+(N-3)*($FS))/TB)'"
    echo "ub820run: matroska: rebuilding video DTS on the $FS s frame grid" >&2
    # (old experiment, UB820_SNAP=2: second copy-only ffmpeg snapping PTS too; it made things worse)
    if [ "${UB820_SNAP:-1}" = 2 ]; then SID="-streamid 0:4113"; i=0; while [ $i -lt "${AN:-0}" ] && [ "$AT" != none ]; do SID="$SID -streamid $((i+1)):$((4352+i))"; i=$((i+1)); done
      SNAP=" | $FFX -hide_banner -loglevel error -nostdin -i - -map 0 -c copy -bsf:v 'setts=pts=round(round(PTS*TB/($FS))*($FS)/TB):dts=round((round(STARTPTS*TB/($FS))+N-3)*($FS)/TB)' $SID -f mpegts -mpegts_flags +resend_headers -pcr_period 20 -"; fi
  fi
fi
# ---- subtitles: pick a source -> SUBFILE (an .srt/.vtt subovl reads; it re-reads it while the extractor grows it)
SRT=/tmp/ub820.srt; SUBFILE=""; SUBX=""; SUBPID=""; EXPID=""; SUBKEY=""
: > /tmp/ub820run.subtracks
SN=0; echo "$INFO" | $BB grep 'Stream #.*Subtitle:' | while read -r SLINE; do
  SC=$(echo "$SLINE" | $BB sed -n 's/.*Subtitle: \([a-z0-9_]*\).*/\1/p')
  SLG=$(echo "$SLINE" | $BB sed -n 's/.*Stream #0:[0-9]*(\([a-z]*\)).*/\1/p')
  case "$SC" in subrip|srt|ass|ssa|mov_text|webvtt|text) K=text;; *) K=bitmap;; esac
  echo "$SN $SC ${SLG:-und} $K" >> /tmp/ub820run.subtracks; SN=$((SN+1))
done
case "$ST" in
  none|"") ;;
  auto) for x in srt SRT vtt VTT; do [ -r "${F%.*}.$x" ] && { SUBFILE="${F%.*}.$x"; break; }; done
        [ -n "$SUBFILE" ] || ST=$($BB awk '$4=="text"{print $1; exit}' /tmp/ub820run.subtracks)
        [ -n "$SUBFILE$ST" ] || echo "ub820run: subtitles auto: no sidecar file and no text subtitle stream" >&2;;
  *[!0-9]*) if [ -r "$ST" ]; then SUBFILE="$ST"; else echo "ub820run: subtitle file $ST not readable" >&2; fi; ST="";;
esac
case "$ST" in ''|*[!0-9]*) ;; *)
  SL=$($BB awk -v n="$ST" '$1==n' /tmp/ub820run.subtracks)
  if [ -z "$SL" ]; then echo "ub820run: no subtitle stream $ST" >&2
  else SCODEC=$(echo "$SL" | $BB awk '{print $2}'); SLANG=$(echo "$SL" | $BB awk '{print $3}')
    if echo "$SL" | $BB grep -q ' bitmap$'; then echo "ub820run: subtitle stream $ST is $SCODEC: bitmap subtitles not supported yet" >&2
    else SUBFILE=$SRT; SUBKEY="$F|$ST|$($BB stat -c %s "$F" 2>/dev/null)"
      if [ -s $SRT ] && [ "$($BB cat $SRT.key 2>/dev/null)" = "$SUBKEY" ]; then echo "ub820run: subtitles: stream $ST ($SCODEC, $SLANG), cached $SRT" >&2
      else NICE=""; $BB nice -n 0 true 2>/dev/null && NICE="$BB nice -n 10"
        # whole-file demux (needed for seeks); runs alongside playback, the overlay picks cues up as they appear
        SUBX="$NICE $FF -hide_banner -loglevel error -nostdin -y -i \"$F\" -map 0:s:$ST -flush_packets 1 -f srt $SRT"
        echo "ub820run: subtitles: stream $ST ($SCODEC, $SLANG), extracting to $SRT in the background" >&2
      fi
    fi
  fi;;
esac
if [ -n "$SUBFILE" ] && [ "$MODE" = --4k ] && [ "${UB820_SUBS_4K:-1}" = 0 ]; then   # 4K route now default for all; overlay draws there (2026-11-28)
  echo "ub820run: subtitles are not supported in 4K mode yet (plane 7 untested there; UB820_SUBS_4K=1 to try)" >&2; SUBFILE=""; SUBX=""; fi
if [ -n "$SUBFILE" ] && [ ! -x $BIN/subovl ]; then echo "ub820run: $BIN/subovl missing: no subtitles" >&2; SUBFILE=""; SUBX=""; fi
echo "ub820run: video=$VC ${W}x${H} -> $CODEC ($( [ "${VOPT#-c:v copy}" != "$VOPT" ] && echo copy || echo transcode)), audio[$AT]=${AC:-none} -> ${ACODEC:-none}, $MODE" >&2
# Pin the streaming ffmpeg to CPU 2 when video is copied (software video transcodes need all cores): with ffmpeg free to run on the cores
# vcavsrv's decoder threads use, live audio transcoding made the video stall ~0.3 s every ~5 s (2026-11-28; nice and
# SCHED_IDLE did not help, pinning did: 0 dips in 60 s). UB820_FFRUN overrides the runlow options ("" = no pinning).
FFR=${UB820_FFRUN-$( [ "${VOPT#-c:v copy}" != "$VOPT" ] && echo "-c 0x4")}   # transcode: all cores (pinned it fell behind)
[ -n "$FFR" ] && [ -x $BIN/runlow ] && FFX="$BIN/runlow $FFR $FF"
CMD="$FFX -hide_banner -loglevel error -nostdin $SSO $RAWIN $DECOPT -i \"$F\" -map 0:v:0 $VOPT $AOPT -f mpegts -mpegts_flags +resend_headers -streamid 0:4113 -pcr_period 20 -$SNAP"
[ -n "${UB820_TEE:-}" ] && CMD="$CMD | $BB head -c ${UB820_TEEMAX:-120000000} | $BB tee $UB820_TEE"   # test knob: capture what ub820play gets
if [ -n "${UB820_DRY:-}" ]; then echo "$CMD | $PL --codec $CODEC $MODE $APL -"
  [ -n "$SUBX" ] && echo "$SUBX &"; [ -n "$SUBFILE" ] && echo "$BIN/subovl \"$SUBFILE\" ${UB820_SUBOPT:-} &"; exit 0; fi
start_subs(){ [ -n "$SUBFILE" ] || return 0
  $BIN/subovl "$SUBFILE" ${UB820_SUBOPT:-} >> /tmp/subovl.log 2>&1 < /dev/null & SUBPID=$!; }
stop_subs(){ [ -n "$SUBPID" ] || return 0; kill "$SUBPID" 2>/dev/null; wait "$SUBPID" 2>/dev/null; SUBPID=""; }
stop_extract(){ [ -n "$EXPID" ] || return 0
  if kill -0 "$EXPID" 2>/dev/null; then kill "$EXPID" 2>/dev/null; wait "$EXPID" 2>/dev/null   # incomplete: no cache key
  elif wait "$EXPID" 2>/dev/null; then echo "$SUBKEY" > $SRT.key; fi; EXPID=""; }
# ---- ub820keys BEGIN: remote-control daemon (IR/CEC/LAN keys -> ub820play ctl; findings/remote-keys.md) ----
# Runs for the whole playback (all renewal sessions); stopped before homerestore and on any exit. UB820_KEYS=0 = off.
KEYPID=""
start_keys(){ [ "${UB820_KEYS:-1}" != 0 ] && [ -x $BIN/ub820keys ] || return 0
  $BIN/ub820keys --log /tmp/ub820keys.log < /dev/null > /dev/null 2>&1 & KEYPID=$!; }
stop_keys(){ [ -n "$KEYPID" ] || return 0; kill "$KEYPID" 2>/dev/null; wait "$KEYPID" 2>/dev/null; KEYPID=""; }
# ---- ub820keys END ----
cleanup(){ stop_keys; stop_subs; stop_extract; [ "${USE_LL:-0}" = 1 ] && $BB umount /usr/local/bin/vcavsrv 2>/dev/null; }   # ub820keys: +stop_keys
trap cleanup EXIT
trap 'exit 143' TERM INT HUP
# ---- native-player recovery BEGIN (findings/native-player-recovery.md) ----
# Panasonic's own media player (apl_medm -> fmpsrv + apl_meda_plynv + flcsrv) holds the decoder (/dev/dtv/tdav*0, rtsc
# ch/buf ports, AVD ch0) while its USB file list is up, even after STOP. Starting us then makes vcavsrv fail
# (vcav_tdav_open_av EBUSY, vcr_open_port, AVD_ERR_STATE, unmute probe), crash in teardown and leave /tmp/vcavsrv.alive:
# every later run then says "vcavsrv not reachable" (the next vcavsrv waits on that lock forever). Our homerestore also
# paints the home over the still-running media app, so it looks like Home but the decoder stays taken.
# Recovery, no reboot: (1) media app alive -> inject the HOME key into apl_mgr (keyinj 0x57 = the IR remote's HOME
# through AM's LAN-key path); AM closes the media app the stock way (AVD_Close, flcsrv term, home). (2) clear a stale
# /tmp/vcavsrv.alive (no multi-threaded vcavsrv alive); release + wait out any run stuck on it. Acts only when needed;
# skipped while ub820play runs. UB820_RECOVER=0 turns it off.
native_holders(){   # pids holding the TDAV / rtsc stream ports (vcavsrv excluded) + a running fmpsrv
  { $BB ls -l /proc/[0-9]*/fd 2>/dev/null | $BB awk '/^\/proc\/[0-9]+\/fd:$/{split($0,a,"/"); p=a[3]; next}
      / -> \/dev\/dtv\/tdav(video|audio)0$| -> \/dev\/dtvrec\/rtsc\/ch[0-9]+$/{print p}'; $BB pidof fmpsrv; } |
    $BB tr ' ' '\n' | $BB sort -u | while read -r p; do
      [ -n "$p" ] && [ -d /proc/$p ] && [ "$($BB cat /proc/$p/comm 2>/dev/null)" != vcavsrv ] && echo "$p"; done; }
vcav_servers(){ for p in $($BB pidof vcavsrv); do   # a real server has many threads, a lock waiter has 1 (unknown = server)
  T=$($BB awk '/^Threads:/{print $2}' /proc/$p/status 2>/dev/null); [ "${T:-2}" -gt 1 ] 2>/dev/null && echo "$p"; done; }
other_runs(){ for p in $($BB pgrep -f ub820run.sh); do [ "$p" = $$ ] && continue
  [ "$($BB awk '/^PPid:/{print $2}' /proc/$p/status 2>/dev/null)" = $$ ] || echo "$p"; done; }
if [ "${UB820_RECOVER:-1}" != 0 ] && ! $BB pidof ub820play >/dev/null; then
  NH=$(native_holders | $BB tr '\n' ' ')
  if [ -n "$NH" ]; then
    echo "ub820run: recovery: Panasonic media player holds the decoder (pids $NH): sending HOME to close it" >&2
    if [ -x $BIN/keyinj ]; then $BIN/keyinj 0x57 >&2; else echo "ub820run: recovery: $BIN/keyinj missing" >&2; fi
    i=0; while [ $i -lt 40 ] && [ -n "$(native_holders)" ]; do $BB sleep 0.5; i=$((i+1)); done
    NH=$(native_holders | $BB tr '\n' ' ')
    if [ -n "$NH" ]; then echo "ub820run: ERROR: recovery: decoder still held by pids $NH after 20 s; not starting" \
      "(leave the Panasonic player with HOME, then retry)" >&2; exit 20; fi
    echo "ub820run: recovery: media player closed after ~$((i/2)) s" >&2; $BB sleep 2   # AM finishes its home transition
  fi
  if [ -e /tmp/vcavsrv.alive ]; then
    # a vcavsrv finishing a normal teardown removes the lock itself: give it 5 s before judging the lock stale
    i=0; while [ $i -lt 10 ] && [ -e /tmp/vcavsrv.alive ] && $BB pidof vcavsrv >/dev/null; do $BB sleep 0.5; i=$((i+1)); done
    if [ -e /tmp/vcavsrv.alive ] && [ -z "$(vcav_servers)" ]; then
      echo "ub820run: recovery: removing stale /tmp/vcavsrv.alive (vcavsrv lock waiters: $($BB pidof vcavsrv))" >&2
      OR=$(other_runs); [ -n "$OR" ] && : > /tmp/ub820run.nohome   # a run stuck on the lock: no home restore when it unwinds
      $BB rm -f /tmp/vcavsrv.alive
      i=0; while [ $i -lt 20 ] && { $BB pidof vcavsrv >/dev/null || [ -n "$(other_runs)" ]; }; do $BB sleep 0.5; i=$((i+1)); done
      [ -n "$(other_runs)" ] && echo "ub820run: recovery: an earlier ub820run.sh is still running ($(other_runs))" >&2
    elif [ -e /tmp/vcavsrv.alive ]; then echo "ub820run: recovery: a vcavsrv is still running ($(vcav_servers)); left alone" >&2; fi
  fi
fi
# ---- native-player recovery END ----
# one overlay at a time: stop leftovers from an earlier run (SIGTERM; they clear LOSD0 on the way out)
if [ -n "$SUBFILE" ] && $BB pidof subovl >/dev/null; then $BB killall subovl 2>/dev/null
  for i in 1 2 3; do $BB pidof subovl >/dev/null || break; $BB sleep 1; done; fi
[ -n "$SUBFILE" ] && : > /tmp/subovl.log         # one log per run (tmpfs)
if [ -n "$SUBX" ]; then $BB rm -f $SRT $SRT.key; eval "exec $SUBX" < /dev/null & EXPID=$!; fi   # exec: EXPID is ffmpeg itself
# HD mode: make the UI plane fully transparent so the video shows full screen (gserver restores it on the next redraw)
[ "$MODE" = --hd ] && [ -x $BIN/flipclear ] && $BIN/flipclear >/dev/null 2>&1
# Session renewal: vcavsrv allows ~300 transfers per session (~1.26 GB with 4 MiB writes). ub820play stops cleanly at
# the limit with rc 12 and records its position; restart from there (a few seconds' gap, home routing restored between).
# Experimental lossless passthrough: bind-mount the patched vcavsrv for this playback only (RAM; undone on exit/reboot)
if [ "${USE_LL:-0}" = 1 ]; then
  $BB grep -q ' /usr/local/bin/vcavsrv ' /proc/mounts || $BB mount --bind ${UB820_LLBIN:-$BIN/vcavsrv.lossless} /usr/local/bin/vcavsrv
  echo "ub820run: LOSSLESS passthrough (patched vcavsrv bound)" >&2
  # (unmounted by cleanup() on exit)
fi
BASE=${SS:-0}; N=0
$BB rm -f /tmp/ub820run.nohome
printf '%s\n%s\n%s\n' "$F" "$AT" "$STARG" > /tmp/ub820run.cur
# duration (s) for the web remote's progress bar and seek clamp
echo "$INFO" | $BB sed -n 's/.*Duration: \([0-9]*\):\([0-9]*\):\([0-9.]*\),.*/\1 \2 \3/p' | $BB head -1 | $BB awk '{printf "%.1f\n", $1*3600+$2*60+$3}' > /tmp/ub820run.dur
start_keys                        # ub820keys: remote control on
while :; do
  echo "$BASE" > /tmp/ub820run.base
  start_subs                      # one overlay per player session: it exits/clears when this session's player is done
  eval "$CMD" | $PL --codec $CODEC $MODE $APL ${UB820_PLAYOPT:-} -
  RC=$?; N=$((N+1))
  stop_subs
  echo "ub820run: player exit $RC (session $N)" >&2
  [ "$RC" = 12 ] && [ "$N" -lt 50 ] || break
  POS=$($BB sed -n 's/.* pos=\([0-9.]*\).*/\1/p' /tmp/ub820play.status)
  [ -n "$POS" ] || break
  BASE=$($BB awk -v b="$BASE" -v p="$POS" 'BEGIN{printf "%.3f", b+p}')
  echo "ub820run: renewing session at ${BASE}s" >&2
  CMD="$FFX -hide_banner -loglevel error -nostdin -ss $BASE $RAWIN $DECOPT -i \"$F\" -map 0:v:0 $VOPT $AOPT -f mpegts -mpegts_flags +resend_headers -streamid 0:4113 -pcr_period 20 -$SNAP"
  [ "$MODE" = --hd ] && [ -x $BIN/flipclear ] && $BIN/flipclear >/dev/null 2>&1
done
# 4K sessions tear the home UI down (4KOTT use case); bring it back: AM "go home" (42004) + ACEMGR ONPLAY.
stop_keys                         # ub820keys: remote control off before the home screen comes back
# (skipped when the web remote / ub820keys restarts us at once for a seek: /tmp/ub820run.nohome)
if [ -f /tmp/ub820run.nohome ]; then $BB rm -f /tmp/ub820run.nohome
elif [ "$MODE" = --4k ] && [ -x $BIN/homerestore ]; then $BIN/homerestore >> /tmp/homerestore.log 2>&1; fi
exit $RC
