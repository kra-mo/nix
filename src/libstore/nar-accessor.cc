#include "nar-accessor.hh"
#include "archive.hh"

#include <map>
#include <stack>
#include <algorithm>

#include <nlohmann/json.hpp>

namespace nix {

struct NarMember
{
    SourceAccessor::Stat stat;

    std::string target;

    /* If this is a directory, all the children of the directory. */
    std::map<std::string, NarMember> children;
};

struct NarAccessor : public SourceAccessor
{
    std::optional<const std::string> nar;

    GetNarBytes getNarBytes;

    NarMember root;

    struct NarIndexer : ParseSink, Source
    {
        NarAccessor & acc;
        Source & source;

        std::stack<NarMember *> parents;

        bool isExec = false;

        uint64_t pos = 0;

        NarIndexer(NarAccessor & acc, Source & source)
            : acc(acc), source(source)
        { }

        void createMember(const Path & path, NarMember member)
        {
            size_t level = std::count(path.begin(), path.end(), '/');
            while (parents.size() > level) parents.pop();

            if (parents.empty()) {
                acc.root = std::move(member);
                parents.push(&acc.root);
            } else {
                if (parents.top()->stat.type != Type::tDirectory)
                    throw Error("NAR file missing parent directory of path '%s'", path);
                auto result = parents.top()->children.emplace(baseNameOf(path), std::move(member));
                parents.push(&result.first->second);
            }
        }

        void createDirectory(const Path & path) override
        {
            createMember(path, {Type::tDirectory, false, 0, 0});
        }

        void createRegularFile(const Path & path) override
        {
            createMember(path, {Type::tRegular, false, 0, 0});
        }

        void closeRegularFile() override
        { }

        void isExecutable() override
        {
            parents.top()->stat.isExecutable = true;
        }

        void preallocateContents(uint64_t size) override
        {
            assert(size <= std::numeric_limits<uint64_t>::max());
            auto & st = parents.top()->stat;
            st.fileSize = (uint64_t) size;
            st.narOffset = pos;
        }

        void receiveContents(std::string_view data) override
        { }

        void createSymlink(const Path & path, const std::string & target) override
        {
            createMember(path,
                NarMember{
                    .stat = {.type = Type::tSymlink},
                    .target = target});
        }

        size_t read(char * data, size_t len) override
        {
            auto n = source.read(data, len);
            pos += n;
            return n;
        }
    };

    NarAccessor(std::string && _nar) : nar(_nar)
    {
        StringSource source(*nar);
        NarIndexer indexer(*this, source);
        parseDump(indexer, indexer);
    }

    NarAccessor(Source & source)
    {
        NarIndexer indexer(*this, source);
        parseDump(indexer, indexer);
    }

    NarAccessor(const std::string & listing, GetNarBytes getNarBytes)
        : getNarBytes(getNarBytes)
    {
        using json = nlohmann::json;

        std::function<void(NarMember &, json &)> recurse;

        recurse = [&](NarMember & member, json & v) {
            std::string type = v["type"];

            if (type == "directory") {
                member.stat = {.type = Type::tDirectory};
                for (auto i = v["entries"].begin(); i != v["entries"].end(); ++i) {
                    std::string name = i.key();
                    recurse(member.children[name], i.value());
                }
            } else if (type == "regular") {
                member.stat = {
                    .type = Type::tRegular,
                    .fileSize = v["size"],
                    .isExecutable = v.value("executable", false),
                    .narOffset = v["narOffset"]
                };
            } else if (type == "symlink") {
                member.stat = {.type = Type::tSymlink};
                member.target = v.value("target", "");
            } else return;
        };

        json v = json::parse(listing);
        recurse(root, v);
    }

    NarMember * find(const CanonPath & path)
    {
        NarMember * current = &root;

        for (auto & i : path) {
            if (current->stat.type != Type::tDirectory) return nullptr;
            auto child = current->children.find(std::string(i));
            if (child == current->children.end()) return nullptr;
            current = &child->second;
        }

        return current;
    }

    NarMember & get(const CanonPath & path) {
        auto result = find(path);
        if (!result)
            throw Error("NAR file does not contain path '%1%'", path);
        return *result;
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto i = find(path);
        if (!i)
            return std::nullopt;
        return i->stat;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto i = get(path);

        if (i.stat.type != Type::tDirectory)
            throw Error("path '%1%' inside NAR file is not a directory", path);

        DirEntries res;
        for (auto & child : i.children)
            res.insert_or_assign(child.first, std::nullopt);

        return res;
    }

    std::string readFile(const CanonPath & path) override
    {
        auto i = get(path);
        if (i.stat.type != Type::tRegular)
            throw Error("path '%1%' inside NAR file is not a regular file", path);

        if (getNarBytes) return getNarBytes(*i.stat.narOffset, *i.stat.fileSize);

        assert(nar);
        return std::string(*nar, *i.stat.narOffset, *i.stat.fileSize);
    }

    std::string readLink(const CanonPath & path) override
    {
        auto i = get(path);
        if (i.stat.type != Type::tSymlink)
            throw Error("path '%1%' inside NAR file is not a symlink", path);
        return i.target;
    }
};

ref<SourceAccessor> makeNarAccessor(std::string && nar)
{
    return make_ref<NarAccessor>(std::move(nar));
}

ref<SourceAccessor> makeNarAccessor(Source & source)
{
    return make_ref<NarAccessor>(source);
}

ref<SourceAccessor> makeLazyNarAccessor(const std::string & listing,
    GetNarBytes getNarBytes)
{
    return make_ref<NarAccessor>(listing, getNarBytes);
}

using nlohmann::json;
json listNar(ref<SourceAccessor> accessor, const CanonPath & path, bool recurse)
{
    auto st = accessor->lstat(path);

    json obj = json::object();

    switch (st.type) {
    case SourceAccessor::Type::tRegular:
        obj["type"] = "regular";
        if (st.fileSize)
            obj["size"] = *st.fileSize;
        if (st.isExecutable)
            obj["executable"] = true;
        if (st.narOffset && *st.narOffset)
            obj["narOffset"] = *st.narOffset;
        break;
    case SourceAccessor::Type::tDirectory:
        obj["type"] = "directory";
        {
            obj["entries"] = json::object();
            json &res2 = obj["entries"];
            for (auto & [name, type] : accessor->readDirectory(path)) {
                if (recurse) {
                    res2[name] = listNar(accessor, path + name, true);
                } else
                    res2[name] = json::object();
            }
        }
        break;
    case SourceAccessor::Type::tSymlink:
        obj["type"] = "symlink";
        obj["target"] = accessor->readLink(path);
        break;
    case SourceAccessor::Type::tMisc:
        assert(false); // cannot happen for NARs
    }
    return obj;
}

}
