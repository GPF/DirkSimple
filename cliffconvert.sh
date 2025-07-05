ffmpeg -err_detect ignore_err \
  -i /mnt/d/Daphne/vldp/cliff/CH_640x480_24p.m2v \
  -i /mnt/d/Daphne/vldp/cliff/CH_640x480_24p.ogg \
  -codec:v libtheora -qscale:v 7 \
  -codec:a libvorbis -qscale:a 5 \
  -pix_fmt yuv420p \
  -y cliff.ogv