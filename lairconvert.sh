ffmpeg -i /mnt/d/Daphne/vldp_dl/lair/lair.m2v -i /mnt/d/Daphne/vldp_dl/lair/lair.og
g \
  -vf scale=512:256 \
  -r 23.976 \
  -c:v libtheora -qscale:v 6 \
  -c:a copy \
  -pix_fmt yuv420p \
  -avoid_negative_ts make_zero \
  lair_dreamcast.ogv
