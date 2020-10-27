#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>
#include <array.h>

static struct cv *cv_traffic;
static struct lock *lock_traffic;
struct array *vehicles;


struct vehicle {
  Direction origin;
  Direction destination;
};

static bool right_turn(struct vehicle *v);
static bool vehicle_allowed(struct vehicle *v);
static struct vehicle *vehicle_create(Direction origin, Direction destination);

struct vehicle *vehicle_create(Direction origin, Direction destination) {
  struct vehicle *v = kmalloc(sizeof(struct vehicle));
  v->origin = origin;
  v->destination = destination;
  return v;
}

bool
right_turn(struct vehicle *v) {
  KASSERT(v != NULL);
  if (((v->origin == west) && (v->destination == south)) ||
      ((v->origin == south) && (v->destination == east)) ||
      ((v->origin == east) && (v->destination == north)) ||
      ((v->origin == north) && (v->destination == west))) {
    return true;
  } else {
    return false;
  }
}

bool
vehicle_allowed(struct vehicle *v) {
  for(unsigned int i = 0; i < array_num(vehicles); i++) {
    struct vehicle *k = array_get(vehicles, i);
    if(v->origin == k->origin)
      continue;
    if(v->origin == k->destination && v->destination == k->origin)
      continue;
    if(v->destination != k->destination && (right_turn(v) || right_turn(k)))
      continue;
    return false;
  }
  return true;
}

void
intersection_sync_init(void)
{
  cv_traffic = cv_create("traffic_cv");
  lock_traffic = lock_create("traffic_lock");
  vehicles = array_create();
  array_init(vehicles);

  KASSERT(cv_traffic != NULL);
  KASSERT(lock_traffic != NULL);
  KASSERT(vehicles != NULL);
}

void
intersection_sync_cleanup(void)
{
  KASSERT(cv_traffic != NULL);
  KASSERT(lock_traffic != NULL);
  KASSERT(vehicles != NULL);
  cv_destroy(cv_traffic);
  lock_destroy(lock_traffic);
  array_destroy(vehicles);
}

void
intersection_before_entry(Direction origin, Direction destination) 
{
  KASSERT(cv_traffic != NULL);
  KASSERT(lock_traffic != NULL);
  KASSERT(vehicles != NULL);

  struct vehicle *v = vehicle_create(origin, destination);
  lock_acquire(lock_traffic);
  while(!vehicle_allowed(v)) {
    cv_wait(cv_traffic, lock_traffic);
  }
  array_add(vehicles, v, NULL);
  lock_release(lock_traffic);
}

void
intersection_after_exit(Direction origin, Direction destination) 
{
  KASSERT(cv_traffic != NULL);
  KASSERT(lock_traffic != NULL);
  KASSERT(vehicles != NULL);

  lock_acquire(lock_traffic);

  for(unsigned int i = 0; i < array_num(vehicles); i++) {
    struct vehicle *k = array_get(vehicles, i);
    if(k->origin == origin && k->destination == destination) {
      array_remove(vehicles, i);
      cv_broadcast(cv_traffic, lock_traffic);
      break;
    }
  }
  
  lock_release(lock_traffic);
}
