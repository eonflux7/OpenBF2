// Path/type names [INFERRED]. PE tables 9497F0/949850/9498B0/94ECE0;
// all implemented instructions read.
#include "NamedObjectRegistry.h"
#if !DEDICATED
#include "Object.h"
#include "Stream.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <unordered_map>
namespace dice { namespace meme {
namespace {

// [PERF] The child index. A container keeps its children in one singly linked list: containers
// first, then object entries sorted by name, and both the lookup and the insert walk it from the
// head (the lookup recursively, one virtual call per entry). The HUD registers some ten thousand
// objects under the root while each hudBuilder command looks names up, so building the HUD is
// quadratic: about 5 s of startup. Here each container that is looked up or inserted into keeps a
// side index of its object entries by name, kept up to date by insert and erase, and both
// operations use it to reach the same entry, or the same position, the walk would. It is used
// only while the list has the shape the walk relies on (containers before objects, objects in
// strcmp order); a list in any other shape keeps the walk. A rename through an entry rebuilds the
// indexes on next use; replacing a container's children drops its index. The layout of the
// registry classes is unchanged (the index lives beside them). BF2_PERF_MEMEINDEX=0 keeps the
// walk everywhere.
bool memeIndexEnabled()
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("BF2_PERF_MEMEINDEX"); on = (e && *e == '0') ? 0 : 1; }
    return on == 1;
}
struct ChildIndex
{
    ChildIndex() : epoch(0), built(false), usable(false) {}
    std::multimap<std::string, NameTreeNode*> objects;   // equal names in list order
    unsigned int epoch;                                  // g_nameEpoch when built
    bool built;
    bool usable;                                         // the list has the indexed shape
};
std::unordered_map<const NameTreeNode*, ChildIndex> g_childIndexes;
unsigned int g_nameEpoch = 0;   // bumped by NameTreeObjectEntry::setName

// The container's index, built from its list when missing or stale; null when the index is off
// or the list does not have the indexed shape.
ChildIndex* childIndex(const NameTreeNode* container, NameTreeNode* children)
{
    if (!memeIndexEnabled())
        return nullptr;
    ChildIndex& index = g_childIndexes[container];
    if (index.built && index.epoch == g_nameEpoch)
        return index.usable ? &index : nullptr;
    index.built = true;
    index.objects.clear();
    index.epoch = g_nameEpoch;
    index.usable = true;
    bool seenObject = false;
    const char* previous = nullptr;
    for (NameTreeNode* node = children; node; node = node->next) {
        if (node->isContainer()) {
            if (seenObject) { index.usable = false; break; }
            continue;
        }
        const char* const name = node->getObject() ? node->getName() : nullptr;
        if (!name || (previous && std::strcmp(previous, name) > 0)) { index.usable = false; break; }
        seenObject = true;
        previous = name;
        index.objects.insert(index.objects.end(), std::make_pair(std::string(name), node));
    }
    if (!index.usable) {
        index.objects.clear();
        return nullptr;
    }
    return &index;
}
void dropChildIndex(const NameTreeNode* container)
{
    if (!g_childIndexes.empty())
        g_childIndexes.erase(container);
}
// Removes an object entry that is being unlinked from the container's list.
void unindexChild(const NameTreeNode* container, NameTreeNode* node)
{
    if (g_childIndexes.empty() || node->isContainer())
        return;
    std::unordered_map<const NameTreeNode*, ChildIndex>::iterator found = g_childIndexes.find(container);
    if (found == g_childIndexes.end() || !found->second.usable)
        return;
    ChildIndex& index = found->second;
    const char* const name = node->getObject() ? node->getName() : nullptr;
    if (name) {
        typedef std::multimap<std::string, NameTreeNode*>::iterator It;
        std::pair<It, It> range = index.objects.equal_range(std::string(name));
        for (It it = range.first; it != range.second; ++it)
            if (it->second == node) { index.objects.erase(it); return; }
    }
    g_childIndexes.erase(found);   // not where its name says: rebuild on next use
}
// The first object entry named path among a container's children, as the walk finds it; sets
// handled to false when the walk must run instead.
Object* findIndexedChild(const NameTreeNode* container, NameTreeNode* children, const char* path,
                         bool& handled)
{
    handled = false;
    ChildIndex* const index = childIndex(container, children);
    if (!index)
        return nullptr;
    handled = true;
    const std::string key(path);
    std::multimap<std::string, NameTreeNode*>::iterator it = index->objects.lower_bound(key);
    return (it == index->objects.end() || it->first != key) ? nullptr : it->second->getObject();
}
char* copyName(const char* name) // 737DC0, ordinary allocation; no null guard on allocation
{
    if (!name) return nullptr;
    const unsigned int size = static_cast<unsigned int>(std::strlen(name)) + 1;
    char* copy = static_cast<char*>(::operator new(size));
    std::memcpy(copy, name, size);
    return copy;
}
const char* separator(const char* name)
{
    if (name) for (; *name; ++name) if (*name == '/' || *name == '\\') return name;
    return nullptr;
}
}
NameTreeNode::~NameTreeNode() { delete next; }
const char* NameTreeNode::getName() { return nullptr; }
bool NameTreeNode::setName(const char*) { return false; }
Object* NameTreeNode::getObject() { return nullptr; }
void NameTreeNode::setPayload(void*) {}
bool NameTreeNode::isContainer() { return false; }
NameTreeNode* NameTreeNode::getChildren() { return nullptr; }
void NameTreeNode::setChildren(NameTreeNode*) {}
bool NameTreeNode::save(System*) { return false; }
bool NameTreeNode::contains(Object*) { return false; }
bool NameTreeNode::insert(System*, Object*, const char*) { return false; }
NameTreeNode* NameTreeNode::createContainer(System*, const char*) { return nullptr; }
bool NameTreeNode::eraseObject(System*, Object*) { return false; }
bool NameTreeNode::eraseNode(System*, NameTreeNode*) { return false; }
bool NameTreeNode::preClose(System*) { return false; }
bool NameTreeNode::close(System*) { return false; }
Object* NameTreeNode::findObject(System*, const char*) { return nullptr; }
NameTreeNode* NameTreeNode::findEntry(System*, Object*) { return nullptr; }
NameTreeNode* NameTreeNode::findChild(const char*) { return nullptr; }
NameTreeNode* NameTreeNode::findParent(Object*) { return nullptr; }
bool NameTreeNode::objectPath(Object*, char*, int) { return true; }
bool NameTreeNode::nodePath(NameTreeNode*, char*, int) { return true; }
bool NameTreeNode::stream(System*, Stream*) { return false; }

NameTreeContainer::NameTreeContainer(const char* name)
    : m_children(nullptr), m_name(copyName(name)) {}
NameTreeContainer::~NameTreeContainer() // name, children, then base-owned next
{ dropChildIndex(this); ::operator delete(m_name); m_name = nullptr; delete m_children; }   // [PERF] the index
const char* NameTreeContainer::getName() { return m_name; }
bool NameTreeContainer::setName(const char* name)
{
    if (!name) return false;
    ::operator delete(m_name); m_name = nullptr;
    m_name = copyName(name);
    return true;
}
void NameTreeContainer::setPayload(void* payload)
{ dropChildIndex(this); m_children = static_cast<NameTreeNode*>(payload); }   // [PERF] the index
bool NameTreeContainer::isContainer() { return true; }
NameTreeNode* NameTreeContainer::getChildren() { return m_children; }
void NameTreeContainer::setChildren(NameTreeNode* children)
{ dropChildIndex(this); m_children = children; }   // [PERF] the index
bool NameTreeContainer::save(System* system) // next is visited even after child failure
{
    bool result = true;
    if (m_children) result = m_children->save(system);
    if (next && !next->save(system)) return false;
    return result;
}
bool NameTreeContainer::contains(Object* object)
{
    if (!object) return false;
    if (m_children && m_children->contains(object)) return true;
    return next ? next->contains(object) : false;
}
bool NameTreeContainer::insert(System* system, Object* object, const char* container)
{
    if (!object) return false;
    const char* name = object->getName();
    if (!name) return false;
    if (container && *container) {
        NameTreeNode* node = createContainer(system, container);
        return node ? node->insert(system, object, nullptr) : false;
    }
    NameTreeNode* previous = nullptr;
    NameTreeNode* current = m_children;
    // [PERF] With the index, the same position: after the last object entry whose name is not
    // greater (upper_bound), or after the leading containers when there is none.
    ChildIndex* const index = childIndex(this, m_children);
    std::multimap<std::string, NameTreeNode*>::iterator at;
    if (index) {
        at = index->objects.upper_bound(std::string(name));
        if (at != index->objects.begin()) {
            std::multimap<std::string, NameTreeNode*>::iterator before = at;
            previous = (--before)->second;
        } else {
            for (NameTreeNode* node = m_children; node && node->isContainer(); node = node->next)
                previous = node;
        }
        current = previous ? previous->next : m_children;
    } else {
        while (current) {
            if (current->getObject() && std::strcmp(current->getName(), name) > 0) break;
            previous = current; current = current->next;
        }
    }
    void* allocation = ::operator new(sizeof(NameTreeObjectEntry));
    NameTreeNode* entry = allocation ? new(allocation) NameTreeObjectEntry(object) : nullptr;
    // The retail null-allocation branch still dereferences entry here.
    if (previous) { entry->next = current; previous->next = entry; }
    else { entry->next = m_children; m_children = entry; }
    if (index)   // [PERF] last among equal names, as in the list
        index->objects.insert(at, std::make_pair(std::string(name), entry));
    return true;
}
NameTreeNode* NameTreeContainer::createContainer(System* system, const char* path)
{
    if (!path || !*path) return nullptr;
    const char* rest = separator(path);
    char name[256];
    if (rest) {
        const unsigned int count = static_cast<unsigned int>(rest - path);
        std::memcpy(name, path, count); name[count] = 0; ++rest;
    } else std::strcpy(name, path);
    NameTreeNode* previous = nullptr;
    NameTreeNode* current = m_children;
    while (current) {
        const int comparison = std::strcmp(current->getName(), name);
        if (!comparison) {
            if (!current->isContainer()) return nullptr;
            return rest ? current->createContainer(system, rest) : current;
        }
        if (comparison > 0 || current->getObject()) break;
        previous = current; current = current->next;
    }
    void* allocation = ::operator new(sizeof(NameTreeContainer));
    NameTreeNode* node = allocation ? new(allocation) NameTreeContainer(name) : nullptr;
    if (previous) { node->next = current; previous->next = node; }
    else { node->next = m_children; m_children = node; }
    return rest ? node->createContainer(system, rest) : node;
}
bool NameTreeContainer::eraseObject(System* system, Object* object)
{
    NameTreeNode* previous = nullptr;
    for (NameTreeNode* node = m_children; node; previous = node, node = node->next) {
        if (node->getObject() == object) {
            unindexChild(this, node);   // [PERF] the index
            if (previous) previous->next = node->next; else m_children = node->next;
            node->next = nullptr; delete node; return true;
        }
        if (node->getChildren() && node->eraseObject(system, object)) return true;
    }
    return false;
}
bool NameTreeContainer::eraseNode(System* system, NameTreeNode* target)
{
    NameTreeNode* previous = nullptr;
    for (NameTreeNode* node = m_children; node; previous = node, node = node->next) {
        if (node == target) {
            unindexChild(this, node);   // [PERF] the index
            if (previous) previous->next = node->next; else m_children = node->next;
            node->next = nullptr; delete node; return true;
        }
        if (node->getChildren() && node->eraseNode(system, target)) return true;
    }
    return false;
}
bool NameTreeContainer::preClose(System* system)
{
    if (m_children && !m_children->preClose(system)) return false;
    if (next && !next->preClose(system)) return false;
    return true;
}
bool NameTreeContainer::close(System* system)
{
    if (m_children && !m_children->close(system)) return false;
    if (next && !next->close(system)) return false;
    return true;
}
Object* NameTreeContainer::findObject(System* system, const char* path)
{
    if (!path) return nullptr;
    if (m_children) {
        if (!m_name) {
            // [PERF] The index answers a plain name; a path walks.
            bool handled = false;
            Object* object = separator(path) ? nullptr : findIndexedChild(this, m_children, path, handled);
            if (!handled) object = m_children->findObject(system, path);
            if (object) return object;
        } else {
            const char* rest = separator(path);
            if (rest) {
                char name[128];
                const unsigned int count = static_cast<unsigned int>(rest - path);
                std::memcpy(name, path, count); name[count] = 0;
                if (!std::strcmp(m_name, name)) {
                    bool handled = false;   // [PERF] as above
                    Object* object = separator(rest + 1)
                        ? nullptr : findIndexedChild(this, m_children, rest + 1, handled);
                    if (!handled) object = m_children->findObject(system, rest + 1);
                    if (object) return object;
                }
            }
        }
    }
    return next ? next->findObject(system, path) : nullptr;
}
NameTreeNode* NameTreeContainer::findEntry(System* system, Object* object)
{
    if (m_children) {
        NameTreeNode* found = m_children->findEntry(system, object);
        if (found) return found;
    }
    return next ? next->findEntry(system, object) : nullptr;
}
NameTreeNode* NameTreeContainer::findChild(const char* name)
{
    for (NameTreeNode* node = m_children; node; node = node->next)
        if (!std::strcmp(node->getName(), name)) return node;
    return nullptr;
}
NameTreeNode* NameTreeContainer::findParent(Object* object)
{
    for (NameTreeNode* node = m_children; node; node = node->next) {
        if (node->isContainer()) {
            NameTreeNode* found = node->findParent(object);
            if (found) return found;
        } else if (node->getObject() == object) return this;
    }
    return nullptr;
}
bool NameTreeContainer::objectPath(Object* object, char* buffer, int capacity)
{
    if (m_children) {
        const unsigned int length = static_cast<unsigned int>(std::strlen(buffer));
        if (m_name) { std::strcpy(buffer + length, m_name); std::strcat(buffer + length, "/"); }
        if (m_children->objectPath(object, buffer, capacity)) return true;
        buffer[length] = 0;
    }
    return next ? next->objectPath(object, buffer, capacity) : false;
}
bool NameTreeContainer::nodePath(NameTreeNode* node, char* buffer, int capacity)
{
    if (node == this) { std::strcat(buffer, m_name); return true; }
    if (m_children) {
        const unsigned int length = static_cast<unsigned int>(std::strlen(buffer));
        if (m_name) { std::strcpy(buffer + length, m_name); std::strcat(buffer + length, "/"); }
        if (m_children->nodePath(node, buffer, capacity)) return true;
        buffer[length] = 0;
    }
    return next ? next->nodePath(node, buffer, capacity) : false;
}
bool NameTreeContainer::stream(System* system, Stream* output)
{
    if (m_children && !m_children->stream(system, output)) return false;
    if (next && !next->stream(system, output)) return false;
    return true;
}
bool NameTreeRoot::close(System* system) // 83AC90, two passes before destroying anything
{
    if (m_children && !m_children->preClose(system)) return false;
    if (next && !next->preClose(system)) return false;
    if (m_children && !m_children->close(system)) return false;
    if (next && !next->close(system)) return false;
    return true;
}
bool NameTreeRoot::objectPath(Object* object, char* buffer, int capacity)
{
    if (!buffer || !capacity) return false;
    buffer[0] = 0;
    if (!object) return false;
    return NameTreeContainer::objectPath(object, buffer, capacity);
}
bool NameTreeRoot::nodePath(NameTreeNode* node, char* buffer, int capacity)
{
    if (!buffer) return false;
    buffer[0] = 0; // unlike objectPath, this happens even when capacity is zero
    if (!capacity || !node) return false;
    return NameTreeContainer::nodePath(node, buffer, capacity);
}
const char* NameTreeObjectEntry::getName() { return m_object->getName(); }
bool NameTreeObjectEntry::setName(const char* name) // 83A220, ignores object return
{ if (!name) return false; ++g_nameEpoch; m_object->setName(name); return true; }   // [PERF] reindex
Object* NameTreeObjectEntry::getObject() { return m_object; }
void NameTreeObjectEntry::setPayload(void* object)
{ ++g_nameEpoch; m_object = static_cast<Object*>(object); }   // [PERF] reindex
bool NameTreeObjectEntry::contains(Object* object) // 83A240 includes null equality
{ return m_object == object || (next ? next->contains(object) : false); }
bool NameTreeObjectEntry::eraseObject(System* system, Object* object)
{ return next ? next->eraseObject(system, object) : false; }
bool NameTreeObjectEntry::eraseNode(System* system, NameTreeNode* node)
{ return next ? next->eraseNode(system, node) : false; }
bool NameTreeObjectEntry::close(System* system)
{
    ++g_nameEpoch;   // [PERF] the object goes; reindex (the walk then meets the entry as retail does)
    if (m_object && !destroyRuntimeObject(reinterpret_cast<RuntimeObject*&>(m_object))) return false;
    if (next && !next->close(system)) return false;
    return true;
}
Object* NameTreeObjectEntry::findObject(System* system, const char* path)
{
    if (!path || separator(path)) return nullptr;
    const int comparison = std::strcmp(m_object->getName(), path);
    if (!comparison) return m_object;
    if (comparison > 0) return nullptr;
    return next ? next->findObject(system, path) : nullptr;
}
NameTreeNode* NameTreeObjectEntry::findEntry(System* system, Object* object)
{ return m_object == object ? this : (next ? next->findEntry(system, object) : nullptr); }
bool NameTreeObjectEntry::objectPath(Object* object, char* buffer, int capacity)
{
    if (m_object == object) { std::strcat(buffer, object->getName()); return true; }
    return next ? next->objectPath(object, buffer, capacity) : false;
}
bool NameTreeObjectEntry::nodePath(NameTreeNode* node, char* buffer, int capacity)
{
    if (this == node) { std::strcat(buffer, m_object->getName()); return true; }
    return next ? next->nodePath(node, buffer, capacity) : false;
}
bool NameTreeObjectEntry::save(System* system) // 83A270, next visited after failure
{
    bool result = true;
    if (m_object && !saveNamedObject(system, m_object)) result = false;
    if (next && !next->save(system)) return false;
    return result;
}
bool NameTreeObjectEntry::preClose(System* system)
{
    if (m_object) {
        DeletionStream deletion(true);
        if (!deletion.streamObject(system, m_object, "Object", nullptr)) return false;
    }
    if (next && !next->preClose(system)) return false;
    return true;
}
bool NameTreeObjectEntry::stream(System* system, Stream* output)
{
    if (m_object && !output->streamObject(system, m_object, "Object", nullptr)) return false;
    return next ? next->stream(system, output) : true;
}
// saveNamedObject is defined in ClassOStream.cpp; System::init owns registry creation.
// [FRONTIER] Specialized visitor descriptor defaults remain outside this registry.
#if defined(_M_IX86)
static_assert(sizeof(NameTreeNode) == 8, "name-tree node layout");
static_assert(sizeof(NameTreeContainer) == 16, "name-tree container layout");
static_assert(sizeof(NameTreeRoot) == 16, "name-tree root layout");
static_assert(sizeof(NameTreeObjectEntry) == 12, "name-tree object entry layout");
#endif
} }
#endif
