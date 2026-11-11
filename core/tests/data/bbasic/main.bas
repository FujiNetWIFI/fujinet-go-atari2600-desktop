 rem batari Basic fixture: loop with gosub and drawscreen
 dim frames = a
 frames = 0

mainloop
 COLUBK = $86
 frames = frames + 1
 gosub bump
 drawscreen
 goto mainloop

bump
 COLUP0 = frames
 return
