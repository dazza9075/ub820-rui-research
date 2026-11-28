#!/bin/busybox sh
# ub820run.subs.sh — ub820run.sh + text subtitles drawn by subovl on the UI plane (LOSD0). Otherwise identical.
# Usage: ub820run.subs.sh <file> [audio_track_index|none] [start_seconds] [subtitle_track|none|auto|/path/file.srt]
#   arg 4 (default none = same behaviour as ub820run.sh):
#     N       subtitle stream N of the file (ffmpeg 0:s:N, counting ALL subtitle streams; list in /tmp/ub820run.subtracks)
#             text codecs (subrip/srt/ass/ssa/mov_text/webvtt/text) are extracted to /tmp/ub820.srt in the background
#             with the static ffmpeg (cached per file+track); bitmap codecs (PGS, DVD, DVB) are not supported yet.
#     auto    a sidecar <file>.srt / .vtt next to the media if present, else the first text subtitle stream
#     PATH    an external .srt / .vtt file
#   Subtitles are drawn in HD mode only (4K: plane 7 state untested; UB820_SUBS_4K=1 to try anyway).
#   UB820_SUBOPT = extra subovl options, e.g. "--size 60 --offset -0.4". subovl logs to /tmp/subovl.log.
#   Probes with the static ffmpeg, then pipes a 188-byte MPEG-TS into ub820play:
#     video: MPEG-1/2, H.264, HEVC, VC-1 -> copied (hardware decode); anything else -> software-decoded and
#            re-encoded to MPEG-2 on the fly (fine for SD, ~real time at 720p).
#     audio: AAC / AC-3 / E-AC-3 / MP2 / MP3 -> copied; DTS, DTS-HD, TrueHD, FLAC, Vorbis, Opus, PCM ... -> AC-3 5.1 640k
#            (vcavsrv's codec table cannot select DTS/TrueHD; see findings/vcav-audio.md).
#     routing: > 1920x1088 -> --4k (4KOTT use case, home UI is torn down), else --hd (home survives; the OSD
#              is cleared to transparent so the video is visible full screen).
# Env: UB820_BIN (dir with ffmpeg/ub820play, default /tmp), UB820_DRY=1 (print the pipeline only).
BB=/bin/busybox; BIN=${UB820_BIN:-/tmp}; FF=$BIN/ffmpeg; PL=$BIN/ub820play
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
MODE=--hd; [ "${W:-0}" -gt 1920 ] || [ "${H:-0}" -gt 1088 ] && MODE=--4k
# audio: map ALL audio tracks (max 4) so they can be switched live (ub820play ctl "audio <pid> <codec>");
# each goes on PID 0x1100+i, copied when vcavsrv can take it, else transcoded to AC-3. The start track is $AT.
AOPT="-an"; APL="--apid none"; AN=$(echo "$INFO" | $BB grep -c 'Stream #.*Audio:'); : > /tmp/ub820run.tracks
[ "$AN" -gt 4 ] && AN=4
if [ "$AT" != none ] && [ "$AN" -gt 0 ]; then
  [ "$AT" -ge "$AN" ] && AT=0
  AOPT=""; i=0
  while [ $i -lt $AN ]; do
    AL=$(echo "$INFO" | $BB grep 'Stream #.*Audio:' | $BB sed -n "$((i+1))p")
    C=$(echo "$AL" | $BB sed -n 's/.*Audio: \([a-z0-9_]*\).*/\1/p')
    LL=0; [ -f /tmp/ub820.lossless ] && [ -x $BIN/vcavsrv.lossless ] && LL=1
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
            X="ac3 -b:a:$i 640k -ar:a:$i 48000 -ac:a:$i $CH";;
    esac
    PID=$((4352+i)); AOPT="$AOPT -map 0:a:$i -c:a:$i $X -streamid $((i+1)):$PID"
    LANG=$(echo "$AL" | $BB sed -n 's/.*Stream #0:[0-9]*\(([a-z]*)\)\{0,1\}.*/\1/p')
    printf '%s 0x%x %s %s %s\n' "$i" "$PID" "$K" "$C" "$LANG" >> /tmp/ub820run.tracks
    [ $i = "$AT" ] && { AC=$C; ACODEC=$K; APL="--apid $(printf 0x%x $PID) --acodec $K"; }
    i=$((i+1))
  done
fi
SSO=""; [ -n "$SS" ] && SSO="-ss $SS"
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
if [ -n "$SUBFILE" ] && [ "$MODE" = --4k ] && [ -z "${UB820_SUBS_4K:-}" ]; then
  echo "ub820run: subtitles are not supported in 4K mode yet (plane 7 untested there; UB820_SUBS_4K=1 to try)" >&2; SUBFILE=""; SUBX=""; fi
if [ -n "$SUBFILE" ] && [ ! -x $BIN/subovl ]; then echo "ub820run: $BIN/subovl missing: no subtitles" >&2; SUBFILE=""; SUBX=""; fi
echo "ub820run: video=$VC ${W}x${H} -> $CODEC ($( [ "$VOPT" = "-c:v copy" ] && echo copy || echo transcode)), audio[$AT]=${AC:-none} -> ${ACODEC:-none}, $MODE" >&2
CMD="$FF -hide_banner -loglevel error -nostdin $SSO $RAWIN -i \"$F\" -map 0:v:0 $VOPT $AOPT -f mpegts -mpegts_flags +resend_headers -streamid 0:4113 -pcr_period 20 -"
if [ -n "${UB820_DRY:-}" ]; then echo "$CMD | $PL --codec $CODEC $MODE $APL -"
  [ -n "$SUBX" ] && echo "$SUBX &"; [ -n "$SUBFILE" ] && echo "$BIN/subovl \"$SUBFILE\" ${UB820_SUBOPT:-} &"; exit 0; fi
start_subs(){ [ -n "$SUBFILE" ] || return 0
  $BIN/subovl "$SUBFILE" ${UB820_SUBOPT:-} >> /tmp/subovl.log 2>&1 < /dev/null & SUBPID=$!; }
stop_subs(){ [ -n "$SUBPID" ] || return 0; kill "$SUBPID" 2>/dev/null; wait "$SUBPID" 2>/dev/null; SUBPID=""; }
stop_extract(){ [ -n "$EXPID" ] || return 0
  if kill -0 "$EXPID" 2>/dev/null; then kill "$EXPID" 2>/dev/null; wait "$EXPID" 2>/dev/null   # incomplete: no cache key
  elif wait "$EXPID" 2>/dev/null; then echo "$SUBKEY" > $SRT.key; fi; EXPID=""; }
cleanup(){ stop_subs; stop_extract; [ "${USE_LL:-0}" = 1 ] && $BB umount /usr/local/bin/vcavsrv 2>/dev/null; }
trap cleanup EXIT
trap 'exit 143' TERM INT HUP
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
  $BB grep -q ' /usr/local/bin/vcavsrv ' /proc/mounts || $BB mount --bind $BIN/vcavsrv.lossless /usr/local/bin/vcavsrv
  echo "ub820run: LOSSLESS passthrough (patched vcavsrv bound)" >&2
  # (unmounted by cleanup() on exit)
fi
BASE=${SS:-0}; N=0
printf '%s\n%s\n%s\n' "$F" "$AT" "$STARG" > /tmp/ub820run.cur
while :; do
  echo "$BASE" > /tmp/ub820run.base
  start_subs                      # one overlay per player session: it exits/clears when this session's player is done
  eval "$CMD" | $PL --codec $CODEC $MODE $APL -
  RC=$?; N=$((N+1))
  stop_subs
  echo "ub820run: player exit $RC (session $N)" >&2
  [ "$RC" = 12 ] && [ "$N" -lt 50 ] || break
  POS=$($BB sed -n 's/.* pos=\([0-9.]*\).*/\1/p' /tmp/ub820play.status)
  [ -n "$POS" ] || break
  BASE=$($BB awk -v b="$BASE" -v p="$POS" 'BEGIN{printf "%.3f", b+p}')
  echo "ub820run: renewing session at ${BASE}s" >&2
  CMD="$FF -hide_banner -loglevel error -nostdin -ss $BASE $RAWIN -i \"$F\" -map 0:v:0 $VOPT $AOPT -f mpegts -mpegts_flags +resend_headers -streamid 0:4113 -pcr_period 20 -"
  [ "$MODE" = --hd ] && [ -x $BIN/flipclear ] && $BIN/flipclear >/dev/null 2>&1
done
# 4K sessions tear the home UI down (4KOTT use case); bring it back: AM "go home" (42004) + ACEMGR ONPLAY.
[ "$MODE" = --4k ] && [ -x $BIN/homerestore ] && $BIN/homerestore >> /tmp/homerestore.log 2>&1
exit $RC
