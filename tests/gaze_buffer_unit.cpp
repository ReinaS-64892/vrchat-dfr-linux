#include "dxvk_gaze_buffer.h"

#include <cassert>
#include <iostream>

using dxvk::GazeVrsBufferState;

int main()
{
  GazeVrsBufferState state;
  std::array<uint64_t, 2> completed = {};
  assert(state.active == -1);
  assert(!state.promote(completed));
  assert(state.writable(completed) == 0);

  // Initial upload must complete before any attachment can be displayed.
  auto firstUpload = state.upload(0);
  assert(!state.promote(completed));
  assert(state.active == -1);
  assert(state.writable(completed) == -1);
  completed[0] = firstUpload;
  assert(state.promote(completed));
  assert(state.active == 0);

  // The front remains usable while the other slot uploads. A pending upload
  // cannot be overwritten even when gaze changes several times in one list.
  auto firstRead = state.read();
  assert(state.writable(completed) == 1);
  auto secondUpload = state.upload(1);
  for (int i = 0; i < 100; ++i)
  {
    assert(!state.promote(completed));
    assert(state.active == 0);
    assert(state.writable(completed) == -1);
  }
  auto lastRead = state.read();
  completed[1] = secondUpload;
  assert(state.promote(completed));
  assert(state.active == 1);

  // Upload completion alone does not make the former front safe to overwrite.
  // ALL previously recorded reads must finish, including the most recent one.
  assert(state.writable(completed) == -1);
  completed[0] = firstRead;
  assert(state.writable(completed) == -1);
  completed[0] = lastRead;
  assert(state.writable(completed) == 0);

  // Repeat ping-pong with several render passes per command list. Values are
  // monotonic per slot; stalled completion never changes the active image.
  for (int i = 0; i < 1000; ++i)
  {
    auto back = state.writable(completed);
    auto front = state.active;
    assert(back >= 0 && back != front);
    auto upload = state.upload(back);
    uint64_t read = 0;
    for (int pass = 0; pass < 8; ++pass)
    {
      read = state.read();
      assert(state.active == front);
      assert(!state.promote(completed));
      assert(state.writable(completed) == -1);
    }
    completed[back] = upload;
    assert(state.promote(completed));
    assert(state.active == back);
    assert(state.writable(completed) == -1);
    completed[front] = read;
    assert(state.writable(completed) == front);
  }
  std::cout << "Gaze double-buffer tests passed\n";
}
