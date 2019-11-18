#include <filesystem>
#include "EntityManager.hpp"

#ifndef KENGINE_MAX_SAVE_PATH_LENGTH
# define KENGINE_MAX_SAVE_PATH_LENGTH 64
#endif

#ifndef KENGINE_MAX_COMPONENT_NAME_LENGTH
# define KENGINE_MAX_COMPONENT_NAME_LENGTH 64
#endif

namespace kengine {
	EntityManager::~EntityManager() {
		_systems.clear(); // Clear here so that system dtors still have access to entities
	}

	Entity EntityManager::getEntity(Entity::ID id) {
		detail::ReadLock l(_entitiesMutex);
		return Entity(id, _entities[id].mask, this);
	}

	EntityView EntityManager::getEntity(Entity::ID id) const {
		detail::ReadLock l(_entitiesMutex);
		return EntityView(id, _entities[id].mask);
	}

	void EntityManager::removeEntity(EntityView e) {
		removeEntity(e.id);
	}

	void EntityManager::removeEntity(Entity::ID id) {
		detail::WriteLock l(_updatesMutex);
		if (_updatesLocked) {
			_removals.push_back({ id });
		}
		else
			doRemove(id);
	}

	void EntityManager::setEntityActive(EntityView e, bool active) {
		setEntityActive(e.id, active);
	}

	void EntityManager::setEntityActive(Entity::ID id, bool active) {
		detail::WriteLock l(_entitiesMutex);
		_entities[id].active = active;
		_entities[id].shouldActivateAfterInit = active;
	}

	struct ComponentIDSave {
		size_t id;
		putils::string<KENGINE_MAX_COMPONENT_NAME_LENGTH> name;
	};

	Entity EntityManager::alloc() {
		{
			detail::ReadLock l(_toReuseMutex);
			if (_toReuse.empty()) {
				detail::WriteLock l(_entitiesMutex);
				const auto id = _entities.size();
				_entities.push_back({ false, 0 });
				return Entity(id, 0, this);
			}
		}

		Entity::ID id;
		{
			detail::WriteLock l(_toReuseMutex);
			if (!_toReuseSorted) {
				std::sort(_toReuse.begin(), _toReuse.end(), std::greater<Entity::ID>());
				_toReuseSorted = true;
			}
			id = _toReuse.back();
			_toReuse.pop_back();
		}

#ifndef KENGINE_NDEBUG
		{
			detail::ReadLock l(_archetypesMutex);
			for (const auto & archetype : _archetypes) {
				detail::ReadLock l(archetype.mutex);
				assert(std::find(archetype.entities.begin(), archetype.entities.end(), id) == archetype.entities.end());
			}
		}

		{
			detail::ReadLock l(_entitiesMutex);
			assert(id < _entities.size());
		}
#endif

		return Entity(id, 0, this);
	}

	void EntityManager::addComponent(Entity::ID id, size_t component) {
		updateHasComponent(id, component, true);
	}

	void EntityManager::removeComponent(Entity::ID id, size_t component) {
		updateHasComponent(id, component, false);
	}

	void EntityManager::updateHasComponent(Entity::ID id, size_t component, bool newHasComponent) {
		{
			detail::WriteLock l(_updatesMutex);
			for (auto & update : _updates)
				if (update.id == id) {
					std::unique_lock<std::mutex> l(update.mutex);
					update.newMask[component] = newHasComponent;
					return;
				}
		}

		Entity::Mask currentMask;
		{
			detail::ReadLock l(_entitiesMutex);
			currentMask = _entities[id].mask;
		}

		auto updatedMask = currentMask;
		updatedMask[component] = newHasComponent;
		updateMask(id, updatedMask);
	}

	void EntityManager::updateMask(Entity::ID id, Entity::Mask newMask, bool ignoreOldMask) {
		detail::WriteLock l(_updatesMutex);
		if (_updatesLocked == 0)
			doUpdateMask(id, newMask, ignoreOldMask);
		else {
			Update update;
			update.id = id;
			update.newMask = newMask;
			update.ignoreOldMask = ignoreOldMask;
			_updates.push_back(std::move(update));
		}
	}

	void EntityManager::doAllUpdates() {
		// _updatesMutex already locked
		for (const auto & update : _updates)
			doUpdateMask(update.id, update.newMask, update.ignoreOldMask);
		_updates.clear();

		for (const auto removal : _removals)
			doRemove(removal.id);
		_removals.clear();
	}

	void EntityManager::doUpdateMask(Entity::ID id, Entity::Mask newMask, bool ignoreOldMask) {
		// _updatesMutex already locked
		const auto oldMask = ignoreOldMask ? 0 : _entities[id].mask;
		assert(newMask != oldMask);

		bool removeDone = oldMask == 0;
		bool addDone = newMask == 0;

		{
			detail::WriteLock l(_archetypesMutex);
			for (auto & collection : _archetypes) {
				if (removeDone && addDone)
					break;

				if (collection.mask == oldMask) {
					const auto size = collection.entities.size();
					if (size > 1) {
						const auto it = std::find(collection.entities.begin(), collection.entities.end(), id);
						std::iter_swap(it, collection.entities.begin() + size - 1);
					}
					collection.entities.pop_back();
					collection.sorted = false;
					removeDone = true;
				}

				if (collection.mask == newMask) {
					collection.entities.emplace_back(id);
					collection.sorted = false;
					addDone = true;
				}
			}

			if (!addDone)
				_archetypes.emplace_back(newMask, id);
		}

		detail::WriteLock l(_entitiesMutex);
		_entities[id].mask = newMask;
	}

	void EntityManager::doRemove(Entity::ID id) {
		SystemManager::removeEntity(getEntity(id));

		Entity::Mask mask;
		{
			detail::ReadLock entities(_entitiesMutex);
			mask = _entities[id].mask;
		}

		{
			detail::ReadLock archetypes(_archetypesMutex);
			for (auto & collection : _archetypes) {
				if (collection.mask == mask) {
					detail::WriteLock archetype(collection.mutex);
					const auto tmp = std::find(collection.entities.begin(), collection.entities.end(), id);
					if (collection.entities.size() > 1) {
						std::swap(*tmp, collection.entities.back());
						collection.sorted = false;
					}
					collection.entities.pop_back();
					break;
				}
			}
		}

		{
			detail::WriteLock entities(_entitiesMutex);
			_entities[id].mask = 0;
			_entities[id].active = false;
			_entities[id].shouldActivateAfterInit = true;
		}

		detail::WriteLock l(_toReuseMutex);
		_toReuse.emplace_back(id);
		_toReuseSorted = false;
	}

	/*
	** Collection
	*/

	EntityManager::EntityCollection EntityManager::getEntities() {
		return EntityCollection{ *this };
	}

	EntityManager::EntityCollection::EntityIterator & EntityManager::EntityCollection::EntityIterator::operator++() {
		++index;
		detail::ReadLock l(em._entitiesMutex);
		while (index < em._entities.size() && (em._entities[index].mask == 0 || !em._entities[index].active))
			++index;
		return *this;
	}

	bool EntityManager::EntityCollection::EntityIterator::operator!=(const EntityIterator & rhs) const {
		// Use `<` as it will only be compared with `end()`, and there is a risk that new entities have been added since `end()` was called
		return index < rhs.index;
	}

	Entity EntityManager::EntityCollection::EntityIterator::operator*() const {
		detail::ReadLock l(em._entitiesMutex);
		return Entity(index, em._entities[index].mask, &em);
	}

	EntityManager::EntityCollection::EntityIterator EntityManager::EntityCollection::begin() const {
		size_t i = 0;
		detail::ReadLock l(em._entitiesMutex);
		while (i < em._entities.size() && (em._entities[i].mask == 0 || !em._entities[i].active))
			++i;
		return EntityIterator{ i, em };
	}

	EntityManager::EntityCollection::EntityIterator EntityManager::EntityCollection::end() const {
		detail::ReadLock l(em._entitiesMutex);
		return EntityIterator{ em._entities.size(), em };
	}

	EntityManager::EntityCollection::EntityCollection(EntityManager & em) : em(em) {
		detail::WriteLock l(em._updatesMutex);
		++em._updatesLocked;
	}

	EntityManager::EntityCollection::~EntityCollection() {
		detail::WriteLock l(em._updatesMutex);
		assert(em._updatesLocked != 0);
		--em._updatesLocked;
		if (em._updatesLocked == 0)
			em.doAllUpdates();
	}
}