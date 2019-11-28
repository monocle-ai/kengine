#pragma once

#include "EntityManager.hpp"
#include "functions/ForEachEntity.hpp"
#include "helpers/TypeHelper.hpp"

namespace kengine {
	namespace detail {
		template<typename Comp>
		void forEachEntity(EntityManager & em, const functions::EntityIteratorFunc & func) {
			for (auto & [e, comp] : em.getEntities<Comp>())
				func(e);
		}

		template<typename Comp>
		void forEachEntityWithout(EntityManager & em, const functions::EntityIteratorFunc & func) {
			for (auto & [e, notComp] : em.getEntities<no<Comp>>())
				func(e);
		}
	}
	
	template<typename Comp>
	void registerComponentEntityIterator(EntityManager & em) {
		auto type = TypeHelper::getTypeEntity<Comp>(em);
		type += functions::ForEachEntity{ detail::forEachEntity<Comp> };
		type += functions::ForEachEntityWithout{ detail::forEachEntityWithout<Comp> };
	}

	template<typename ... Comps>
	void registerComponentEntityIterators(EntityManager & em) {
		putils::for_each_type<Comps...>([&](auto type) {
			using Type = putils_wrapped_type(type);
			registerComponentEntityIterator<Type>(em);
		});
	}
}