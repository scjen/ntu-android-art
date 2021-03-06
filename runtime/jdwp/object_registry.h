/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_JDWP_OBJECT_REGISTRY_H_
#define ART_RUNTIME_JDWP_OBJECT_REGISTRY_H_

#include <stdint.h>

#include <map>

#include "jdwp/jdwp.h"
#include "mirror/art_field-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "object_callbacks.h"
#include "safe_map.h"

namespace art {

struct ObjectRegistryEntry {
  // Is jni_reference a weak global or a regular global reference?
  jobjectRefType jni_reference_type;

  // The reference itself.
  jobject jni_reference;

  // A reference count, so we can implement DisposeObject.
  int32_t reference_count;

  // The corresponding id, so we only need one map lookup in Add.
  JDWP::ObjectId id;
};
std::ostream& operator<<(std::ostream& os, const ObjectRegistryEntry& rhs);

// Tracks those objects currently known to the debugger, so we can use consistent ids when
// referring to them. Normally we keep JNI weak global references to objects, so they can
// still be garbage collected. The debugger can ask us to retain objects, though, so we can
// also promote references to regular JNI global references (and demote them back again if
// the debugger tells us that's okay).
class ObjectRegistry {
 public:
  ObjectRegistry();

  JDWP::ObjectId Add(mirror::Object* o) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  JDWP::RefTypeId AddRefType(mirror::Class* c) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template<typename T> T Get(JDWP::ObjectId id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (id == 0) {
      return NULL;
    }
    return reinterpret_cast<T>(InternalGet(id));
  }

  bool Contains(mirror::Object* o) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Clear() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void DisableCollection(JDWP::ObjectId id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void EnableCollection(JDWP::ObjectId id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsCollected(JDWP::ObjectId id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void DisposeObject(JDWP::ObjectId id, uint32_t reference_count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Returned by Get when passed an invalid object id.
  static mirror::Object* const kInvalidObject;

  // This is needed to get the jobject instead of the Object*.
  // Avoid using this and use standard Get when possible.
  jobject GetJObject(JDWP::ObjectId id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Visit, objects are treated as system weaks.
  void UpdateObjectPointers(IsMarkedCallback* callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // We have allow / disallow functionality since we use system weak sweeping logic to update moved
  // objects inside of the object_to_entry_ map.
  void AllowNewObjects() LOCKS_EXCLUDED(lock_);
  void DisallowNewObjects() LOCKS_EXCLUDED(lock_);

 private:
  JDWP::ObjectId InternalAdd(mirror::Object* o) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::Object* InternalGet(JDWP::ObjectId id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void Demote(ObjectRegistryEntry& entry) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, lock_);
  void Promote(ObjectRegistryEntry& entry) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, lock_);

  Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  bool allow_new_objects_ GUARDED_BY(lock_);
  ConditionVariable condition_ GUARDED_BY(lock_);

  std::map<mirror::Object*, ObjectRegistryEntry*> object_to_entry_ GUARDED_BY(lock_);
  SafeMap<JDWP::ObjectId, ObjectRegistryEntry*> id_to_entry_ GUARDED_BY(lock_);

  size_t next_id_ GUARDED_BY(lock_);
};

}  // namespace art

#endif  // ART_RUNTIME_JDWP_OBJECT_REGISTRY_H_
