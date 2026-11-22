# Frames: the late skip and pacing

At a vblank the emulation hands the frame's work to the render thread and goes on, and the GPU runs
a frame or two behind.

## The late skip

When the GPU is three frames behind at a vblank, the next frame leaves out the draws of its busier
screen (by its draws and vertices over the last frames) and keeps the other's, so the game keeps
its speed and only the shown frame rate drops. A draw's screen is that of the LCD buffer it draws
into, or the one a transfer from its buffer reached last; a buffer of no screen goes with the
screens whose draws sample it. A screen is left out at most the frame skip limit's frames in a row
(two by default); then the frame is drawn whole, and the vblank waits until the GPU has finished
the frame four back, so it keeps the frames after that queued. A screen left out shows its last
picture again: a present never shows a GPU picture of a screen older than the one it showed last.

## Presents and pacing

- **Three display buffers.** A present is dropped when two newer ones are queued, unless none has
  been shown for 33 ms. One within 12 ms of the last, or while the last is still on the GPU, waits,
  and a newer one takes its place. With vsync on the display takes the next buffer at its vblank;
  off, at once, and it tears.
- **Pacing.** With vsync on the speed limit is set aside, and each guest frame waits for the
  refresh after its predecessor's: one frame a refresh, a heavy frame borrowing from the light ones
  after it. The emulation is never held more than two refreshes behind, so a game under full speed
  does not wait at all. With vsync off the speed limit paces the frames.
