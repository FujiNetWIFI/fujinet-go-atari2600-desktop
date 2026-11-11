 set romsize 8k
 dim frames = a
 frames = 0

mainloop
 COLUBK = $44
 gosub bump bank2
 drawscreen
 goto mainloop

 bank 2
bump
 frames = frames + 1
 COLUP0 = frames
 return otherbank
