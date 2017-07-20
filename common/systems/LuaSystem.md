# LuaSystem

`System` that executes lua scripts, either at a global scale or attached to a `GameObject` through a [LuaComponent](../components/LuaComponent.md).

### Members

##### Constructor

The constructor automatically defines the following functions for lua scripts:

* `getGameObjects()`: returns all the `GameObjects` currently in existence.
* `createEntity(string type, string name)`
* `removeEntity(GameObject go)`
* `getEntity(string name)`

The [GameObject](../../GameObject.md) type is also registered.

##### registerType

```
template<typename T>
void registerType() noexcept;
```
Registers the [Reflectible](../../putils/reflection/README.md) type `T` with the underlying lua state.

This allows lua scripts to access any attribute or method in `T`'s `Reflectible` API.

This also defines the following function for lua scripts:
* `sendT(T packet)` (e.g. `sendLog(Log packet)`): sends `packet` as a datapacket to all `Systems`

If `T` is a `Component`, the following member functions is added to the `GameObject` lua type for lua scripts:
* `getT()` (e.g. `getMetaComponent()`)
* `hasT()` (e.g. `hasMetaComponent()`)

This allows lua scripts to perform any operation on `GameObjects` if the necessary types are registered. Client code can either give full access to lua scripts by registering all its types (and therefore having a fully extensible game that can be developed almost entirely in lua), or only register a small set of types and/or members, to restrict what modders can do.

##### registerTypes

```
template<typename ...Types>
void registerTypes() noexcept;
```

For each type in `Types`, call `registerType` for it.

##### addScriptDirectory

```
void addScriptDirectory(std::string_view dir) noexcept;
```
Adds `dir` as one of the directories in which to execute lua scripts. Each frame, all the files in `dir` will be interpreted as lua scripts and executed.