#ifndef _H_LINESKY
#define _H_LINESKY


#include <stdint.h>

#ifndef LINESKY_STRIP_STRUCTURES
typedef struct {
  uint16_t x, y;
} ls_vec2d;

typedef struct {
  ls_vec2d* skyline;
  ls_vec2d size;

  bool _init;
  uint16_t _nskyline;
} ls_atlas2d;
#endif 

void ls_atlas_init(ls_atlas2d* atlas, ls_vec2d atlas_dimension);
bool ls_atlas_push_rect(ls_atlas2d* atlas, float* posx, float* posy, ls_vec2d size);

#ifdef LINESKY_IMPLEMENTATION

void ls_atlas_init(ls_atlas2d* atlas, ls_vec2d atlas_dimension) {
  if(!atlas) return;
  memset(atlas, 0, sizeof(ls_atlas2d));
  atlas->size = atlas_dimension;
  atlas->skyline = malloc(sizeof(ls_vec2d) * atlas_dimension.x);
  if(!atlas->skyline) return;
  atlas->skyline[atlas->_nskyline++] = (ls_vec2d){.x = 0, .y = 0};
  atlas->_init = true;
}
bool ls_atlas_push_rect(ls_atlas2d* atlas, float* posx, float* posy, ls_vec2d size) {
  if(!atlas->_init || size.x == 0 || size.y == 0) return false;

  uint16_t maxw   = atlas->size.x;
  uint16_t maxh   = atlas->size.y;
  uint16_t width  = size.x;
  uint16_t height = size.y;

  uint16_t i_best = UINT16_MAX, i_first_after_rect   = UINT16_MAX;
  uint16_t x_best = UINT16_MAX, y_best              = UINT16_MAX;

  for(uint16_t i = 0; i < atlas->_nskyline; i++) {
    uint16_t x = atlas->skyline[i].x;
    uint16_t y = atlas->skyline[i].y;

    if(x + width > maxw) break;
    if(y >= y_best) continue;

    uint16_t rect_endpoint = x + width; 
    uint16_t inext;
    for(inext = i + 1; inext < atlas->_nskyline; inext++) {
      if(rect_endpoint <= atlas->skyline[inext].x) break;
      if(atlas->skyline[inext].y > y) 
        y = atlas->skyline[inext].y;
    }

    if(y >= y_best) continue;
    if(y + height > maxh) continue;

    i_best = i; i_first_after_rect = inext;
    x_best = x; y_best = y;
  }
  if(i_best == UINT16_MAX) return false;

  ls_vec2d last_under_rect = (i_first_after_rect != UINT16_MAX && i_first_after_rect > 0)
    ? atlas->skyline[i_first_after_rect - 1]
    : (ls_vec2d){.x = 0, .y = 0};

  ls_vec2d first_after_rect = (i_first_after_rect < atlas->_nskyline) 
    ? atlas->skyline[i_first_after_rect] 
    : (ls_vec2d){.x = atlas->size.x, .y = 0};

  ls_vec2d new_topleft, new_bottomright;
  new_topleft.x = x_best;
  new_topleft.y = y_best + height;
  new_bottomright.x = x_best + width;
  new_bottomright.y = last_under_rect.y;
  bool need_bottom_right = i_first_after_rect < atlas->_nskyline ? new_bottomright.x < first_after_rect.x : new_bottomright.x < maxw; 


  uint16_t to_remove = i_first_after_rect - i_best;
  uint16_t to_insert = 1 + (uint16_t)need_bottom_right;

  if (atlas->_nskyline + (to_insert - to_remove) > atlas->size.x) return false;

  if(to_insert > to_remove) { 
    int16_t last_old = atlas->_nskyline - 1;
    int16_t new_last_old = last_old + (to_insert - to_remove);
    for(; last_old >= i_first_after_rect; last_old--, new_last_old--) {
      atlas->skyline[new_last_old] = atlas->skyline[last_old];
    }
    atlas->_nskyline += (to_insert - to_remove);
  } else if(to_insert < to_remove) {
    uint16_t shift = to_remove - to_insert;
    for (uint16_t i = i_first_after_rect; i + shift < atlas->_nskyline; i++) {
      atlas->skyline[i - shift] = atlas->skyline[i];
    }
    atlas->_nskyline -= shift;
  }


  atlas->skyline[i_best] = new_topleft;
  if (need_bottom_right && i_best + 1 < atlas->size.x) {
    atlas->skyline[i_best + 1] = new_bottomright;
  }

  *posx = x_best;
  *posy = y_best;
  return true;
}

#endif
#endif

