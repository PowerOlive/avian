#include "heap.h"
#include "system.h"
#include "common.h"

using namespace vm;

namespace {

// an object must survive TenureThreshold + 2 garbage collections
// before being copied to gen2 (must be at least 1):
const unsigned TenureThreshold = 3;

const unsigned FixieTenureThreshold = TenureThreshold + 2;

const unsigned Top = ~static_cast<unsigned>(0);

const unsigned InitialGen2CapacityInBytes = 4 * 1024 * 1024;

const bool Verbose = false;
const bool Verbose2 = false;
const bool Debug = false;
const bool DebugFixies = false;

class Context;

void NO_RETURN abort(Context*);
#ifndef NDEBUG
void assert(Context*, bool);
#endif

System* system(Context*);

inline void*
get(void* o, unsigned offsetInWords)
{
  return mask(cast<void*>(o, offsetInWords * BytesPerWord));
}

inline void**
getp(void* o, unsigned offsetInWords)
{
  return &cast<void*>(o, offsetInWords * BytesPerWord);
}

inline void
set(void** o, void* value)
{
  *o = reinterpret_cast<void*>
    (reinterpret_cast<uintptr_t>(value)
     | reinterpret_cast<uintptr_t>(*o) & (~PointerMask));
}

inline void
set(void* o, unsigned offsetInWords, void* value)
{
  set(getp(o, offsetInWords), value);
}

class Segment {
 public:
  class Map {
   public:
    class Iterator {
     public:
      Map* map;
      unsigned index;
      unsigned limit;
      
      Iterator(Map* map, unsigned start, unsigned end):
        map(map)
      {
        assert(map->segment->context, map->bitsPerRecord == 1);
        assert(map->segment->context, map->segment);
        assert(map->segment->context, start <= map->segment->position());

        if (end > map->segment->position()) end = map->segment->position();

        index = map->indexOf(start);
        limit = map->indexOf(end);

        if ((end - start) % map->scale) ++ limit;
      }

      bool hasMore() {
        unsigned word = wordOf(index);
        unsigned bit = bitOf(index);
        unsigned wordLimit = wordOf(limit);
        unsigned bitLimit = bitOf(limit);

        for (; word <= wordLimit and (word < wordLimit or bit < bitLimit);
             ++word)
        {
          uintptr_t* p = map->data() + word;
          if (*p) {
            for (; bit < BitsPerWord and (word < wordLimit or bit < bitLimit);
                 ++bit)
            {
              if (map->data()[word] & (static_cast<uintptr_t>(1) << bit)) {
                index = ::indexOf(word, bit);
//                 printf("hit at index %d\n", index);
                return true;
              } else {
//                 printf("miss at index %d\n", indexOf(word, bit));
              }
            }
          }
          bit = 0;
        }

        index = limit;

        return false;
      }
      
      unsigned next() {
        assert(map->segment->context, hasMore());
        assert(map->segment->context, map->segment);

        return (index++) * map->scale;
      }
    };

    Segment* segment;
    Map* child;
    unsigned bitsPerRecord;
    unsigned scale;
    bool clearNewData;

    Map(Segment* segment, unsigned bitsPerRecord, unsigned scale,
        Map* child, bool clearNewData):
      segment(segment),
      child(child),
      bitsPerRecord(bitsPerRecord),
      scale(scale),
      clearNewData(clearNewData)
    { }

    void init() {
      assert(segment->context, bitsPerRecord);
      assert(segment->context, scale);
      assert(segment->context, powerOfTwo(scale));

      if (clearNewData) {
        memset(data(), 0, size() * BytesPerWord);
      }

      if (child) {
        child->init();
      }
    }

    void replaceWith(Map* m) {
      assert(segment->context, bitsPerRecord == m->bitsPerRecord);
      assert(segment->context, scale == m->scale);

      m->segment = 0;
      
      if (child) child->replaceWith(m->child);
    }

    unsigned offset(unsigned capacity) {
      unsigned n = 0;
      if (child) n += child->footprint(capacity);
      return n;
    }

    unsigned offset() {
      return offset(segment->capacity());
    }

    uintptr_t* data() {
      return segment->data + segment->capacity() + offset();
    }

    unsigned size(unsigned capacity) {
      unsigned result
        = ceiling(ceiling(capacity, scale) * bitsPerRecord, BitsPerWord);
      assert(segment->context, result);
      return result;
    }

    unsigned size() {
      return size(max(segment->capacity(), 1));
    }

    unsigned indexOf(unsigned segmentIndex) {
      return (segmentIndex / scale) * bitsPerRecord;
    }

    unsigned indexOf(void* p) {
      assert(segment->context, segment->almostContains(p));
      assert(segment->context, segment->capacity());
      return indexOf(segment->indexOf(p));
    }

    void update(uintptr_t* newData, unsigned capacity) {
      assert(segment->context, capacity >= segment->capacity());

      uintptr_t* p = newData + offset(capacity);
      if (segment->position()) {
        memcpy(p, data(), size(segment->position()) * BytesPerWord);
      }

      if (child) {
        child->update(newData, capacity);
      }
    }

    void clearBit(unsigned i) {
      assert(segment->context, wordOf(i) < size());

      vm::clearBit(data(), i);
    }

    void setBit(unsigned i) {
      assert(segment->context, wordOf(i) < size());

      vm::markBit(data(), i);
    }

    void clearOnlyIndex(unsigned index) {
      for (unsigned i = index, limit = index + bitsPerRecord; i < limit; ++i) {
        clearBit(i);
      }
    }

    void clearOnly(unsigned segmentIndex) {
      clearOnlyIndex(indexOf(segmentIndex));
    }

    void clearOnly(void* p) {
      clearOnlyIndex(indexOf(p));
    }

    void clear(void* p) {
      clearOnly(p);
      if (child) child->clear(p);
    }

    void setOnlyIndex(unsigned index, unsigned v = 1) {
      unsigned i = index + bitsPerRecord - 1;
      while (true) {
        if (v & 1) setBit(i); else clearBit(i);
        v >>= 1;
        if (i == index) break;
        --i;
      }
    }

    void setOnly(unsigned segmentIndex, unsigned v = 1) {
      setOnlyIndex(indexOf(segmentIndex), v);
    }

    void setOnly(void* p, unsigned v = 1) {
      setOnlyIndex(indexOf(p), v);
    }

    void set(void* p, unsigned v = 1) {
      setOnly(p, v);
      assert(segment->context, get(p) == v);
      if (child) child->set(p, v);
    }

    unsigned get(void* p) {
      unsigned index = indexOf(p);
      unsigned v = 0;
      for (unsigned i = index, limit = index + bitsPerRecord; i < limit; ++i) {
        unsigned wi = bitOf(i);
        v <<= 1;
        v |= ((data()[wordOf(i)]) & (static_cast<uintptr_t>(1) << wi)) >> wi;
      }
      return v;
    }

    unsigned footprint(unsigned capacity) {
      unsigned n = size(capacity);
      if (child) n += child->footprint(capacity);
      return n;
    }
  };

  Context* context;
  uintptr_t* data;
  unsigned position_;
  unsigned capacity_;
  Map* map;

  Segment(Context* context, Map* map, unsigned desired, unsigned minimum):
    context(context),
    data(0),
    position_(0),
    capacity_(0),
    map(map)
  {
    if (desired) {
      assert(context, desired >= minimum);

      capacity_ = desired;
      while (data == 0) {
        data = static_cast<uintptr_t*>
          (system(context)->tryAllocate
           ((capacity_ + map->footprint(capacity_)) * BytesPerWord));

        if (data == 0) {
          if (capacity_ > minimum) {
            capacity_ = avg(minimum, capacity_);
            if (capacity_ == 0) {
              break;
            }
          } else {
            abort(context);
          }
        }
      }

      if (map) {
        map->init();
      }
    }
  }

  unsigned capacity() {
    return capacity_;
  }

  unsigned position() {
    return position_;
  }

  unsigned remaining() {
    return capacity() - position();
  }

  void replaceWith(Segment* s) {
    if (data) system(context)->free(data);
    data = s->data;
    s->data = 0;

    position_ = s->position_;
    s->position_ = 0;

    capacity_ = s->capacity_;
    s->capacity_ = 0;

    if (s->map) {
      if (map) {
        map->replaceWith(s->map);
        s->map = 0;
      } else {
        abort(context);
      }
    } else {
      assert(context, map == 0);
    }    
  }

  bool contains(void* p) {
    return position() and p >= data and p < data + position();
  }

  bool almostContains(void* p) {
    return contains(p) or p == data + position();
  }

  void* get(unsigned offset) {
    assert(context, offset <= position());
    return data + offset;
  }

  unsigned indexOf(void* p) {
    assert(context, almostContains(p));
    return static_cast<uintptr_t*>(p) - data;
  }

  void* allocate(unsigned size) {
    assert(context, size);
    assert(context, position() + size <= capacity());

    void* p = data + position();
    position_ += size;
    return p;
  }

  void dispose() {
    system(context)->free(data);
    data = 0;
    map = 0;
  }
};

class Fixie {
 public:
  Fixie(unsigned size, bool hasMask, Fixie** handle):
    age(0),
    hasMask(hasMask),
    marked(false),
    dirty(false)
  {
    memset(mask(size), 0, maskSize(size, hasMask));
    add(handle);
    if (DebugFixies) {
      fprintf(stderr, "make fixie %p\n", this);
    }
  }

  void add(Fixie** handle) {
    this->handle = handle;
    next = *handle;
    if (next) next->handle = &next;
    *handle = this;
  }

  void remove() {
    *handle = next;
    if (next) next->handle = handle;
  }

  void move(Fixie** handle) {
    if (DebugFixies) {
      fprintf(stderr, "move fixie %p\n", this);
    }

    remove();
    add(handle);
  }

  void** body() {
    return static_cast<void**>(static_cast<void*>(body_));
  }

  uintptr_t* mask(unsigned size) {
    return body_ + size;
  }

  static unsigned maskSize(unsigned size, bool hasMask) {
    return hasMask * ceiling(size, BytesPerWord) * BytesPerWord;
  }

  static unsigned totalSize(unsigned size, bool hasMask) {
    return sizeof(Fixie) + (size * BytesPerWord) + maskSize(size, hasMask);
  }

  unsigned totalSize(unsigned size) {
    return totalSize(size, hasMask);
  }

  uint8_t age;
  bool hasMask;
  bool marked;
  bool dirty;
  Fixie* next;
  Fixie** handle;
  uintptr_t body_[0];
};

Fixie*
fixie(void* body)
{
  return static_cast<Fixie*>(body) - 1;
}

void
free(Context* c, Fixie** fixies);

class Context {
 public:
  Context(System* system, Heap::Client* client):
    system(system),
    client(client),

    ageMap(&gen1, max(1, log(TenureThreshold)), 1, 0, false),
    gen1(this, &ageMap, 0, 0),

    nextAgeMap(&nextGen1, max(1, log(TenureThreshold)), 1, 0, false),
    nextGen1(this, &nextAgeMap, 0, 0),

    pointerMap(&gen2, 1, 1, 0, true),
    pageMap(&gen2, 1, LikelyPageSizeInBytes / BytesPerWord, &pointerMap, true),
    heapMap(&gen2, 1, pageMap.scale * 1024, &pageMap, true),
    gen2(this, &heapMap, 0, 0),

    nextPointerMap(&nextGen2, 1, 1, 0, true),
    nextPageMap(&nextGen2, 1, LikelyPageSizeInBytes / BytesPerWord,
                &nextPointerMap, true),
    nextHeapMap(&nextGen2, 1, nextPageMap.scale * 1024, &nextPageMap, true),
    nextGen2(this, &nextHeapMap, 0, 0),

    gen2Base(0),
    tenureFootprint(0),
    fixieTenureFootprint(0),
    gen1padding(0),
    gen2padding(0),
    mode(Heap::MinorCollection),

    fixies(0),
    tenuredFixies(0),
    dirtyFixies(0),
    markedFixies(0),
    visitedFixies(0),

    lastCollectionTime(system->now()),
    totalCollectionTime(0),
    totalTime(0)
  { }

  void dispose() {
    gen1.dispose();
    nextGen1.dispose();
    gen2.dispose();
    nextGen2.dispose();
    free(this, &tenuredFixies);
    free(this, &dirtyFixies);
    free(this, &fixies);
    client->dispose();
  }

  System* system;
  Heap::Client* client;
  
  Segment::Map ageMap;
  Segment gen1;

  Segment::Map nextAgeMap;
  Segment nextGen1;

  Segment::Map pointerMap;
  Segment::Map pageMap;
  Segment::Map heapMap;
  Segment gen2;

  Segment::Map nextPointerMap;
  Segment::Map nextPageMap;
  Segment::Map nextHeapMap;
  Segment nextGen2;

  unsigned gen2Base;
  
  unsigned tenureFootprint;
  unsigned fixieTenureFootprint;
  unsigned gen1padding;
  unsigned gen2padding;

  Heap::CollectionType mode;

  Fixie* fixies;
  Fixie* tenuredFixies;
  Fixie* dirtyFixies;
  Fixie* markedFixies;
  Fixie* visitedFixies;

  int64_t lastCollectionTime;
  int64_t totalCollectionTime;
  int64_t totalTime;
};

inline System*
system(Context* c)
{
  return c->system;
}

const char*
segment(Context* c, void* p)
{
  if (c->gen1.contains(p)) {
    return "gen1";
  } else if (c->nextGen1.contains(p)) {
    return "nextGen1";
  } else if (c->gen2.contains(p)) {
    return "gen2";
  } else if (c->nextGen2.contains(p)) {
    return "nextGen2";
  } else {
    return "none";
  }
}

inline void NO_RETURN
abort(Context* c)
{
  abort(c->system);
}

#ifndef NDEBUG
inline void
assert(Context* c, bool v)
{
  assert(c->system, v);
}
#endif

inline void
initNextGen1(Context* c, unsigned footprint)
{
  new (&(c->nextAgeMap)) Segment::Map
    (&(c->nextGen1),  max(1, log(TenureThreshold)), 1, 0, false);

  unsigned minimum
    = (c->gen1.position() - c->tenureFootprint) + footprint + c->gen1padding;
  unsigned desired = minimum;

  new (&(c->nextGen1)) Segment(c, &(c->nextAgeMap), desired, minimum);

  if (Verbose2) {
    fprintf(stderr, "init nextGen1 to %d bytes\n",
            c->nextGen1.capacity() * BytesPerWord);
  }
}

inline bool
oversizedGen2(Context* c)
{
  return c->gen2.capacity() > (InitialGen2CapacityInBytes / BytesPerWord)
    and c->gen2.position() < (c->gen2.capacity() / 4);
}

inline void
initNextGen2(Context* c)
{
  new (&(c->nextPointerMap)) Segment::Map
    (&(c->nextGen2), 1, 1, 0, true);

  new (&(c->nextPageMap)) Segment::Map
    (&(c->nextGen2), 1, LikelyPageSizeInBytes / BytesPerWord,
     &(c->nextPointerMap), true);

  new (&(c->nextHeapMap)) Segment::Map
    (&(c->nextGen2), 1, c->pageMap.scale * 1024, &(c->nextPageMap), true);

  unsigned minimum = c->gen2.position() + c->tenureFootprint + c->gen2padding;
  unsigned desired = minimum;

  if (not oversizedGen2(c)) {
    desired *= 2;
  }

  if (desired < InitialGen2CapacityInBytes / BytesPerWord) {
    desired = InitialGen2CapacityInBytes / BytesPerWord;
  }

  new (&(c->nextGen2)) Segment(c, &(c->nextHeapMap), desired, minimum);

  if (Verbose2) {
    fprintf(stderr, "init nextGen2 to %d bytes\n",
            c->nextGen2.capacity() * BytesPerWord);
  }
}

inline bool
fresh(Context* c, void* o)
{
  return c->nextGen1.contains(o)
    or c->nextGen2.contains(o)
    or (c->gen2.contains(o) and c->gen2.indexOf(o) >= c->gen2Base);
}

inline bool
wasCollected(Context* c, void* o)
{
  return o and (not fresh(c, o)) and fresh(c, get(o, 0));
}

inline void*
follow(Context* c UNUSED, void* o)
{
  assert(c, wasCollected(c, o));
  return cast<void*>(o, 0);
}

inline void*&
parent(Context* c UNUSED, void* o)
{
  assert(c, wasCollected(c, o));
  return cast<void*>(o, BytesPerWord);
}

inline uintptr_t*
bitset(Context* c UNUSED, void* o)
{
  assert(c, wasCollected(c, o));
  return &cast<uintptr_t>(o, BytesPerWord * 2);
}

void
free(Context* c, Fixie** fixies)
{
  for (Fixie** p = fixies; *p;) {
    Fixie* f = *p;
    *p = f->next;
    if (DebugFixies) {
      fprintf(stderr, "free fixie %p\n", f);
    }
    c->system->free(f);
  }
}

void
sweepFixies(Context* c)
{
  assert(c, c->markedFixies == 0);

  if (c->mode == Heap::MajorCollection) {
    free(c, &(c->tenuredFixies));
    free(c, &(c->dirtyFixies));    
  }
  free(c, &(c->fixies));

  for (Fixie** p = &(c->visitedFixies); *p;) {
    Fixie* f = *p;
    *p = f->next;

    unsigned size = c->client->sizeInWords(f->body());

    ++ f->age;
    if (f->age > FixieTenureThreshold) {
      f->age = FixieTenureThreshold;
    } else if (static_cast<unsigned>(f->age + 1) == FixieTenureThreshold) {
      c->fixieTenureFootprint += f->totalSize(size);
    }

    if (f->age == FixieTenureThreshold) {
      if (DebugFixies) {
        fprintf(stderr, "tenure fixie %p (dirty: %d)\n", f, f->dirty);
      }

      if (f->dirty) {
        f->move(&(c->dirtyFixies));
      } else {
        f->move(&(c->tenuredFixies));
      }
    } else {
      f->move(&(c->fixies));
    }

    f->marked = false;
  }
}

inline void*
copyTo(Context* c, Segment* s, void* o, unsigned size)
{
  assert(c, s->remaining() >= size);
  void* dst = s->allocate(size);
  c->client->copy(o, dst);
  return dst;
}

void*
copy2(Context* c, void* o)
{
  unsigned size = c->client->copiedSizeInWords(o);

  if (c->gen2.contains(o)) {
    assert(c, c->mode == Heap::MajorCollection);

    return copyTo(c, &(c->nextGen2), o, size);
  } else if (c->gen1.contains(o)) {
    unsigned age = c->ageMap.get(o);
    if (age == TenureThreshold) {
      if (c->mode == Heap::MinorCollection) {
        assert(c, c->gen2.remaining() >= size);

        if (c->gen2Base == Top) {
          c->gen2Base = c->gen2.position();
        }

        return copyTo(c, &(c->gen2), o, size);
      } else {
        return copyTo(c, &(c->nextGen2), o, size);
      }
    } else {
      o = copyTo(c, &(c->nextGen1), o, size);

      c->nextAgeMap.setOnly(o, age + 1);
      if (age + 1 == TenureThreshold) {
        c->tenureFootprint += size;
      }

      return o;
    }
  } else {
    assert(c, not c->nextGen1.contains(o));
    assert(c, not c->nextGen2.contains(o));

    o = copyTo(c, &(c->nextGen1), o, size);

    c->nextAgeMap.clear(o);

    return o;
  }
}

void*
copy(Context* c, void* o)
{
  void* r = copy2(c, o);

  if (Debug) {
    fprintf(stderr, "copy %p (%s) to %p (%s)\n",
            o, segment(c, o), r, segment(c, r));
  }

  // leave a pointer to the copy in the original
  cast<void*>(o, 0) = r;

  return r;
}

void*
update3(Context* c, void* o, bool* needsVisit)
{
  if (c->client->isFixed(o)) {
    Fixie* f = fixie(o);
    if ((not f->marked)
        and (c->mode == Heap::MajorCollection
             or f->age < FixieTenureThreshold))
    {
      if (DebugFixies) {
        fprintf(stderr, "mark fixie %p\n", f);
      }
      f->marked = true;
      f->move(&(c->markedFixies));
    }
    *needsVisit = false;
    return o;
  } else if (wasCollected(c, o)) {
    *needsVisit = false;
    return follow(c, o);
  } else {
    *needsVisit = true;
    return copy(c, o);
  }
}

void*
update2(Context* c, void* o, bool* needsVisit)
{
  if (c->mode == Heap::MinorCollection and c->gen2.contains(o)) {
    *needsVisit = false;
    return o;
  }

  return update3(c, o, needsVisit);
}

void*
update(Context* c, void** p, bool* needsVisit)
{
  if (mask(*p) == 0) {
    *needsVisit = false;
    return 0;
  }

  return update2(c, mask(*p), needsVisit);
}

void
updateHeapMap(Context* c, void* p, void* target, unsigned offset, void* result)
{
  Segment* seg;
  Segment::Map* map;

  if (c->mode == Heap::MinorCollection) {
    seg = &(c->gen2);
    map = &(c->heapMap);
  } else {
    seg = &(c->nextGen2);
    map = &(c->nextHeapMap);
  }

  if (not (c->client->isFixed(result)
           and fixie(result)->age == FixieTenureThreshold)
      and not seg->contains(result))
  {
    if (target and c->client->isFixed(target)) {
      Fixie* f = fixie(target);
      assert(c, f->hasMask);

      if (static_cast<unsigned>(f->age + 1) >= FixieTenureThreshold) {
        unsigned size = c->client->sizeInWords(f->body());

        if (DebugFixies) {
          fprintf(stderr, "dirty fixie %p at %d\n", f, offset);
        }

        f->dirty = true;
        markBit(f->mask(size), offset);
      }
    } else if (seg->contains(p)) {
      if (Debug) {        
        fprintf(stderr, "mark %p (%s) at %p (%s)\n",
                result, segment(c, result), p, segment(c, p));
      }

      map->set(p);
    }
  }
}

void*
update(Context* c, void** p, void* target, unsigned offset, bool* needsVisit)
{
  if (mask(*p) == 0) {
    *needsVisit = false;
    return 0;
  }

  void* result = update2(c, mask(*p), needsVisit);

  if (result) {
    updateHeapMap(c, p, target, offset, result);
  }

  return result;
}

const uintptr_t BitsetExtensionBit
= (static_cast<uintptr_t>(1) << (BitsPerWord - 1));

void
bitsetInit(uintptr_t* p)
{
  memset(p, 0, BytesPerWord);
}

void
bitsetClear(uintptr_t* p, unsigned start, unsigned end)
{
  if (end < BitsPerWord - 1) {
    // do nothing
  } else if (start < BitsPerWord - 1) {
    memset(p + 1, 0, (wordOf(end + (BitsPerWord * 2) + 1)) * BytesPerWord);
  } else {
    unsigned startWord = wordOf(start + (BitsPerWord * 2) + 1);
    unsigned endWord = wordOf(end + (BitsPerWord * 2) + 1);
    if (endWord > startWord) {
      memset(p + startWord + 1, 0, (endWord - startWord) * BytesPerWord);
    }
  }
}

void
bitsetSet(uintptr_t* p, unsigned i, bool v)
{
  if (i >= BitsPerWord - 1) {
    i += (BitsPerWord * 2) + 1;
    if (v) {
      p[0] |= BitsetExtensionBit;
      if (p[2] <= wordOf(i) - 3) p[2] = wordOf(i) - 2;
    }
  }

  if (v) {
    markBit(p, i);
  } else {
    clearBit(p, i);
  }
}

bool
bitsetHasMore(uintptr_t* p)
{
  switch (*p) {
  case 0: return false;

  case BitsetExtensionBit: {
    uintptr_t length = p[2];
    uintptr_t word = wordOf(p[1]);
    for (; word < length; ++word) {
      if (p[word + 3]) {
        p[1] = indexOf(word, 0);
        return true;
      }
    }
    p[1] = indexOf(word, 0);
    return false;
  }

  default: return true;
  }
}

unsigned
bitsetNext(Context* c, uintptr_t* p)
{
  bool more UNUSED = bitsetHasMore(p);
  assert(c, more);

  switch (*p) {
  case 0: abort(c);

  case BitsetExtensionBit: {
    uintptr_t i = p[1];
    uintptr_t word = wordOf(i);
    assert(c, word < p[2]);
    for (uintptr_t bit = bitOf(i); bit < BitsPerWord; ++bit) {
      if (p[word + 3] & (static_cast<uintptr_t>(1) << bit)) {
        p[1] = indexOf(word, bit) + 1;
        bitsetSet(p, p[1] + BitsPerWord - 2, false);
        return p[1] + BitsPerWord - 2;
      }
    }
    abort(c);
  }

  default: {
    for (unsigned i = 0; i < BitsPerWord - 1; ++i) {
      if (*p & (static_cast<uintptr_t>(1) << i)) {
        bitsetSet(p, i, false);
        return i;
      }
    }
    abort(c);
  }
  }
}

void
collect(Context* c, void** p, void* target, unsigned offset)
{
  void* original = mask(*p);
  void* parent = 0;
  
  if (Debug) {
    fprintf(stderr, "update %p (%s) at %p (%s)\n",
            mask(*p), segment(c, *p), p, segment(c, p));
  }

  bool needsVisit;
  set(p, update(c, mask(p), target, offset, &needsVisit));

  if (Debug) {
    fprintf(stderr, "  result: %p (%s) (visit? %d)\n",
            mask(*p), segment(c, *p), needsVisit);
  }

  if (not needsVisit) return;

 visit: {
    void* copy = follow(c, original);

    class Walker : public Heap::Walker {
     public:
      Walker(Context* c, void* copy, uintptr_t* bitset):
        c(c),
        copy(copy),
        bitset(bitset),
        first(0),
        second(0),
        last(0),
        visits(0),
        total(0)
      { }

      virtual bool visit(unsigned offset) {
        if (Debug) {
          fprintf(stderr, "  update %p (%s) at %p - offset %d from %p (%s)\n",
                  get(copy, offset),
                  segment(c, get(copy, offset)),
                  getp(copy, offset),
                  offset,
                  copy,
                  segment(c, copy));
        }

        bool needsVisit;
        void* childCopy = update
          (c, getp(copy, offset), copy, offset, &needsVisit);
        
        if (Debug) {
          fprintf(stderr, "    result: %p (%s) (visit? %d)\n",
                  childCopy, segment(c, childCopy), needsVisit);
        }

        ++ total;

        if (total == 3) {
          bitsetInit(bitset);
        }

        if (needsVisit) {
          ++ visits;

          if (visits == 1) {
            first = offset;
          } else if (visits == 2) {
            second = offset;
          }
        } else {
          set(copy, offset, childCopy);
        }

        if (visits > 1 and total > 2 and (second or needsVisit)) {
          bitsetClear(bitset, last, offset);
          last = offset;

          if (second) {
            bitsetSet(bitset, second, true);
            second = 0;
          }
          
          if (needsVisit) {
            bitsetSet(bitset, offset, true);
          }
        }

        return true;
      }

      Context* c;
      void* copy;
      uintptr_t* bitset;
      unsigned first;
      unsigned second;
      unsigned last;
      unsigned visits;
      unsigned total;
    } walker(c, copy, bitset(c, original));

    if (Debug) {
      fprintf(stderr, "walk %p (%s)\n", copy, segment(c, copy));
    }

    c->client->walk(copy, &walker);

    if (walker.visits) {
      // descend
      if (walker.visits > 1) {
        ::parent(c, original) = parent;
        parent = original;
      }

      original = get(copy, walker.first);
      set(copy, walker.first, follow(c, original));
      goto visit;
    } else {
      // ascend
      original = parent;
    }
  }

  if (original) {
    void* copy = follow(c, original);

    class Walker : public Heap::Walker {
     public:
      Walker(Context* c, uintptr_t* bitset):
        c(c),
        bitset(bitset),
        next(0),
        total(0)
      { }

      virtual bool visit(unsigned offset) {
        switch (++ total) {
        case 1:
          return true;

        case 2:
          next = offset;
          return true;
          
        case 3:
          next = bitsetNext(c, bitset);
          return false;

        default:
          abort(c);
        }
      }

      Context* c;
      uintptr_t* bitset;
      unsigned next;
      unsigned total;
    } walker(c, bitset(c, original));

    if (Debug) {
      fprintf(stderr, "scan %p\n", copy);
    }

    c->client->walk(copy, &walker);

    assert(c, walker.total > 1);

    if (walker.total == 3 and bitsetHasMore(bitset(c, original))) {
      parent = original;
    } else {
      parent = ::parent(c, original);
    }

    if (Debug) {
      fprintf(stderr, "  next is %p (%s) at %p - offset %d from %p (%s)\n",
              get(copy, walker.next),
              segment(c, get(copy, walker.next)),
              getp(copy, walker.next),
              walker.next,
              copy,
              segment(c, copy));
    }

    original = get(copy, walker.next);
    set(copy, walker.next, follow(c, original));
    goto visit;
  } else {
    return;
  }
}

void
collect(Context* c, void** p)
{
  collect(c, p, 0, 0);
}

void
collect(Context* c, void* target, unsigned offset)
{
  collect(c, getp(target, offset), target, offset);
}

void
visitDirtyFixies(Context* c)
{
  for (Fixie** p = &(c->dirtyFixies); *p;) {
    Fixie* f = *p;

    bool wasDirty = false;
    bool stillDirty = false;
    unsigned size = c->client->sizeInWords(f->body());
    uintptr_t* mask = f->mask(size);

    unsigned word = 0;
    unsigned bit = 0;
    unsigned wordLimit = wordOf(size);
    unsigned bitLimit = bitOf(size);

    for (; word <= wordLimit and (word < wordLimit or bit < bitLimit);
         ++ word)
    {
      if (mask[word]) {
        for (; bit < BitsPerWord and (word < wordLimit or bit < bitLimit);
             ++ bit)
        {
          unsigned index = indexOf(word, bit);

          if (getBit(mask, index)) {
            wasDirty = true;

            clearBit(mask, index);
    
            if (DebugFixies) {
              fprintf(stderr, "clean fixie %p at %d\n", f, index);
            }

            collect(c, f->body(), index);

            if (getBit(mask, index)) {
              stillDirty = true;
            }
          }
        }
        bit = 0;
      }
    }

    assert(c, wasDirty);

    if (stillDirty) {
      p = &(f->next);
    } else {
      f->dirty = false;
      *p = f->next;
      f->move(&(c->tenuredFixies));
    } 
  }
}

void
visitMarkedFixies(Context* c)
{
  for (Fixie** p = &(c->markedFixies); *p;) {
    Fixie* f = *p;
    *p = f->next;

    if (DebugFixies) {
      fprintf(stderr, "visit fixie %p\n", f);
    }

    class Walker: public Heap::Walker {
     public:
      Walker(Context* c, void** p):
        c(c), p(p)
      { }

      virtual bool visit(unsigned offset) {
        collect(c, p, offset);
        return true;
      }

      Context* c;
      void** p;
    } w(c, f->body());

    c->client->walk(f->body(), &w);

    f->move(&(c->visitedFixies));
  }  
}

void
collect(Context* c, Segment::Map* map, unsigned start, unsigned end,
        bool* dirty, bool expectDirty UNUSED)
{
  bool wasDirty = false;
  for (Segment::Map::Iterator it(map, start, end); it.hasMore();) {
    wasDirty = true;
    if (map->child) {
      assert(c, map->scale > 1);
      unsigned s = it.next();
      unsigned e = s + map->scale;

      map->clearOnly(s);
      bool childDirty = false;
      collect(c, map->child, s, e, &childDirty, true);
      if (childDirty) {
        map->setOnly(s);
        *dirty = true;
      }
    } else {
      assert(c, map->scale == 1);
      void** p = reinterpret_cast<void**>(map->segment->get(it.next()));

      map->clearOnly(p);
      if (c->nextGen1.contains(*p)) {
        map->setOnly(p);
        *dirty = true;
      } else {
        collect(c, p);

        if (not c->gen2.contains(*p)) {
          map->setOnly(p);
          *dirty = true;
        }
      }
    }
  }

  assert(c, wasDirty or not expectDirty);
}

void
collect2(Context* c)
{
  c->gen2Base = Top;
  c->tenureFootprint = 0;
  c->fixieTenureFootprint = 0;
  c->gen1padding = 0;
  c->gen2padding = 0;

  if (c->mode == Heap::MinorCollection and c->gen2.position()) {
    unsigned start = 0;
    unsigned end = start + c->gen2.position();
    bool dirty;
    collect(c, &(c->heapMap), start, end, &dirty, false);
  }

  if (c->mode == Heap::MinorCollection) {
    visitDirtyFixies(c);
  }

  class Visitor : public Heap::Visitor {
   public:
    Visitor(Context* c): c(c) { }

    virtual void visit(void* p) {
      collect(c, static_cast<void**>(p));
      visitMarkedFixies(c);
    }

    Context* c;
  } v(c);

  c->client->visitRoots(&v);
}

void
collect(Context* c, unsigned footprint)
{
  if (oversizedGen2(c)
      or c->tenureFootprint + c->gen2padding > c->gen2.remaining()
      or c->fixieTenureFootprint)
  {
    c->mode = Heap::MajorCollection;
  }

  int64_t then;
  if (Verbose) {
    if (c->mode == Heap::MajorCollection) {
      fprintf(stderr, "major collection\n");
    } else {
      fprintf(stderr, "minor collection\n");
    }

    then = c->system->now();
  }

  initNextGen1(c, footprint);
  if (c->mode == Heap::MajorCollection) {
    initNextGen2(c);
  }

  collect2(c);

  c->gen1.replaceWith(&(c->nextGen1));
  if (c->mode == Heap::MajorCollection) {
    c->gen2.replaceWith(&(c->nextGen2));
  }

  sweepFixies(c);

  if (Verbose) {
    int64_t now = c->system->now();
    int64_t collection = now - then;
    int64_t run = then - c->lastCollectionTime;
    c->totalCollectionTime += collection;
    c->totalTime += collection + run;
    c->lastCollectionTime = now;

    fprintf(stderr,
            " - collect: %4"LLD"ms; "
            "total: %4"LLD"ms; "
            "run: %4"LLD"ms; "
            "total: %4"LLD"ms\n",
            collection,
            c->totalCollectionTime,
            run,
            c->totalTime - c->totalCollectionTime);

    fprintf(stderr,
            " - gen1: %8d/%8d bytes; "
            "gen2: %8d/%8d bytes\n",
            c->gen1.position() * BytesPerWord,
            c->gen1.capacity() * BytesPerWord,
            c->gen2.position() * BytesPerWord,
            c->gen2.capacity() * BytesPerWord);
  }
}

class MyHeap: public Heap {
 public:
  MyHeap(System* system, Heap::Client* client): c(system, client) { }

  virtual void collect(CollectionType type, unsigned footprint) {
    c.mode = type;

    ::collect(&c, footprint);
  }

  virtual void* allocateFixed(unsigned sizeInWords, bool objectMask,
                              unsigned* totalInBytes)
  {
    *totalInBytes = Fixie::totalSize(sizeInWords, objectMask);
    return (new (c.system->allocate(*totalInBytes))
            Fixie(sizeInWords, objectMask, &(c.fixies)))->body();
  }

  virtual bool needsMark(void* p) {
    if (c.client->isFixed(p)) {
      return fixie(p)->age == FixieTenureThreshold;
    } else {
      return c.gen2.contains(p);
    }
  }

  bool targetNeedsMark(void* target) {
    return target
      and not c.gen2.contains(target)
      and not (c.client->isFixed(target)
               and fixie(target)->age == FixieTenureThreshold);
  }

  virtual void mark(void* p, unsigned offset, unsigned count) {
    if (c.client->isFixed(p)) {
      Fixie* f = fixie(p);
      assert(&c, f->hasMask);

      unsigned size = c.client->sizeInWords(f->body());

      for (unsigned i = 0; i < count; ++i) {
        void** target = static_cast<void**>(p) + offset + i;
        if (targetNeedsMark(*target)) {
          if (DebugFixies) {
            fprintf(stderr, "dirty fixie %p at %d\n", f, offset + i);
          }

          f->dirty = true;
          markBit(f->mask(size), offset + i);
        }
      }

      if (f->dirty) {
        f->move(&(c.dirtyFixies));
      }
    } else {
      for (unsigned i = 0; i < count; ++i) {
        void** target = static_cast<void**>(p) + offset + i;
        if (targetNeedsMark(*target)) {
          c.heapMap.set(target);
        }
      }
    }
  }

  virtual void pad(void* p, unsigned extra) {
    if (c.gen1.contains(p)) {
      if (c.ageMap.get(p) == TenureThreshold) {
        c.gen2padding += extra;
      } else {
        c.gen1padding += extra;
      }
    } else if (c.gen2.contains(p)) {
      c.gen2padding += extra;
    } else {
      c.gen1padding += extra;
    }
  }

  virtual void* follow(void* p) {
    if (p == 0 or c.client->isFixed(p)) {
      return p;
    } else if (wasCollected(&c, p)) {
      if (Debug) {
        fprintf(stderr, "follow %p (%s) to %p (%s)\n",
                p, segment(&c, p),
                ::follow(&c, p), segment(&c, ::follow(&c, p)));
      }

      return ::follow(&c, p);
    } else {
      return p;
    }
  }

  virtual Status status(void* p) {
    p = mask(p);

    if (p == 0) {
      return Null;
    } else if (c.nextGen1.contains(p)) {
      return Reachable;
    } else if (c.nextGen2.contains(p)
               or (c.gen2.contains(p)
                   and (c.mode == Heap::MinorCollection
                        or c.gen2.indexOf(p) >= c.gen2Base)))
    {
      return Tenured;
    } else if (wasCollected(&c, p)) {
      return Reachable;
    } else {
      return Unreachable;
    }
  }

  virtual CollectionType collectionType() {
    return c.mode;
  }

  virtual void dispose() {
    c.dispose();
    c.system->free(this);
  }

  Context c;
};

} // namespace

namespace vm {

Heap*
makeHeap(System* system, Heap::Client* client)
{  
  return new (system->allocate(sizeof(MyHeap))) MyHeap(system, client);
}

} // namespace vm
