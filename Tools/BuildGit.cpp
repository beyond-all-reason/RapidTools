#include "Rapid/Hex.hpp"
#include "Rapid/LastGit.hpp"
#include "Rapid/PoolArchive.hpp"
#include "Rapid/PoolFile.hpp"
#include "Rapid/ScopeGuard.hpp"
#include "Rapid/Store.hpp"
#include "Rapid/String.hpp"
#include "Rapid/Versions.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <git2.h>
#include <sys/types.h>
#include <sys/stat.h>

namespace {

using namespace Rapid;

void checkRet(int Error, char const * Message, std::string const & Extra = "")
{
	git_error const * LogToError;
	char const * LogToErrorMessage = "";
	char const * LogToErrorSpacer = "";

	if (!Error) return;

	if ((LogToError = giterr_last()) != nullptr && LogToError->message != nullptr)
	{
		LogToErrorMessage = LogToError->message;
		LogToErrorSpacer = " - ";
	}

	std::string Concat;
	if (!Extra.empty())
	{
		Concat = concat(Message, " '", Extra, "' [", std::to_string(Error), "]", LogToErrorSpacer, LogToErrorMessage);
	}
	else
	{
		Concat = concat(Message, " [", std::to_string(Error), ']', LogToErrorSpacer, LogToErrorMessage);
	}
	throw std::runtime_error{Concat};
}

// Replace all instances of $VERSION in a string by calling a functor with substrings
template<typename FunctorT>
void replaceVersion(char const * Buffer, std::size_t BufferSize, std::string const & Replacement, FunctorT Functor)
{
	auto Pos = Buffer;
	auto Last = Buffer + BufferSize;
	std::string VersionString{"$VERSION"};

	while (true)
	{
		auto NewPos = std::search(Pos, Last, VersionString.begin(), VersionString.end());
		if (NewPos == Last) break;
		auto Size = NewPos - Pos;
		if (Size == 0) continue;
		Functor(Pos, Size);
		Functor(Replacement.data(), Replacement.size());
		Pos = NewPos + VersionString.size();
	}

	auto Size = Last - Pos;
	if (Size == 0) return;
	Functor(Pos, Size);
}

struct CommitInfoT
{
	std::string Branch;
	std::string Version;
	bool MakeZip;
};

CommitInfoT extractVersion(std::string const & Log, std::string const & RevisionString)
{
	std::string const StableString{"STABLE"};
	std::string const VersionString{"VERSION{"};

	if (
		Log.size() >= StableString.size() &&
		std::equal(StableString.begin(), StableString.end(), Log.begin()))
	{
		return {"stable", concat("stable-", RevisionString), true};
	}
	else if (
		Log.size() >= VersionString.size() &&
		std::equal(VersionString.begin(), VersionString.end(), Log.begin()))
	{
		auto First = Log.begin() + VersionString.size();
		auto Last = Log.end();
		auto EndPos = std::find(First, Last, '}');
		if (EndPos != Last) return {"stable", {First, EndPos}, true};
	}

	return {"test", concat("test-", RevisionString), false};
}

void convertTreeishToTree(git_tree** Tree, git_repository* Repo, std::string const & Treeish)
{
	git_object * Object;
	checkRet(git_revparse_single(&Object, Repo, Treeish.c_str()), "git_revparse_single", Treeish);
	auto && ObjectGuard = makeScopeGuard([&] { git_object_free(Object); });
	checkRet(git_object_peel(reinterpret_cast<git_object * *>(Tree), Object, GIT_OBJ_TREE), "git_object_peel");
}

std::string concatPrefix(const std::string& prefix, const std::string& path)
{
	if (prefix.empty())
		return path;

	return concat(prefix, "/", path);
}

typedef std::unordered_map<std::string, std::pair<std::string, std::string>> SubmoduleHashes;
struct SubmoduleContext {
	StoreT& Store;
	PoolArchiveT& Archive;
	const std::string& ModRoot;
	const std::string& pathPrefix;
	SubmoduleHashes submoduleHashes;
};

void processDiff(git_diff *Diff, StoreT& Store, PoolArchiveT& Archive, git_repository* Repo, SubmoduleHashes& submoduleHashes, const std::string pathPrefix)
{
	auto && DiffGuard = makeScopeGuard([&] { git_diff_free(Diff); });

	// Helper function used for adds/modifications
	auto add = [&](git_diff_delta const * Delta, const std::string& fullPath)
	{
		PoolFileT File{Store};
		git_blob * Blob;

		checkRet(git_blob_lookup(&Blob, Repo, &Delta->new_file.id), "git_blob_lookup");
		auto && BlobGuard = makeScopeGuard([&] { git_blob_free(Blob); });
		auto Size = git_blob_rawsize(Blob);
		auto Pointer = static_cast<char const *>(git_blob_rawcontent(Blob));
		File.write(Pointer, Size);
		auto FileEntry = File.close();
		Archive.add(fullPath, FileEntry);
	};

	// Update the archive with this diff
	for (std::size_t I = 0, E = git_diff_num_deltas(Diff); I != E; ++I)
	{
		auto Delta = git_diff_get_delta(Diff, I);
		const std::string& fullPath = concatPrefix(pathPrefix, Delta->new_file.path);

		switch (Delta->status) {
			case GIT_DELTA_ADDED: {
				switch(Delta->new_file.mode) {
					case GIT_FILEMODE_BLOB_EXECUTABLE:
					case GIT_FILEMODE_BLOB: {
						std::cout << "A\t" << fullPath << "\n";
						add(Delta, fullPath);
					} break;
					case GIT_FILEMODE_COMMIT: {
						char buffer[128];
						git_oid_tostr(buffer, 128, &Delta->new_file.id);
						submoduleHashes[std::string(Delta->new_file.path)] = {"" , buffer};
						std::cout << "A\t" << fullPath << " " << buffer << "\n";
					} break;
					default: {
						std::stringstream err;
						err << "GIT_DELTA_ADDED: Unsupported mode for " << fullPath << ": " << Delta->new_file.mode;
						throw std::runtime_error{err.str()};
					}
				}
			} break;

			case GIT_DELTA_MODIFIED:
			{
				switch(Delta->new_file.mode) {
					case GIT_FILEMODE_BLOB_EXECUTABLE:
					case GIT_FILEMODE_BLOB: {
						std::cout << "M\t" << fullPath << "\n";
						add(Delta, fullPath);
					} break;
					case GIT_FILEMODE_COMMIT: {
						char buffer1[128];
						char buffer2[128];
						git_oid_tostr(buffer1, 128, &Delta->old_file.id);
						git_oid_tostr(buffer2, 128, &Delta->new_file.id);
						submoduleHashes[std::string(Delta->new_file.path)] = {buffer1 , buffer2};
						std::cout << "M\t" << fullPath << " " << buffer1 << " => " << buffer2 << "\n";
					} break;
					default: {
						std::stringstream err;
						err << "GIT_DELTA_ADDED: Unsupported mode for " << fullPath << ": " << Delta->new_file.mode;
						throw std::runtime_error{err.str()};
					}
				}
			} break;

			case GIT_DELTA_DELETED:
			{
				switch(Delta->old_file.mode) {
					case GIT_FILEMODE_BLOB_EXECUTABLE:
					case GIT_FILEMODE_BLOB: {
						std::cout << "D\t" << fullPath << "\n";
						Archive.remove(Delta->new_file.path);
					} break;
					case GIT_FILEMODE_COMMIT: {
						std::cout << "D\t" << fullPath << " (submodule)\n";
						Archive.removePrefix(fullPath);
					} break;
					default: {
						std::stringstream err;
						err << "GIT_DELTA_ADDED: Unsupported mode for " << fullPath << ": " << Delta->new_file.mode;
						throw std::runtime_error{err.str()};
					}
				}
			} break;

			default: throw std::runtime_error{"Unsupported delta"};
		}
	}
}

void processRepo(StoreT& Store, PoolArchiveT& Archive, git_repository* Repo, const std::string& ModRoot, const std::string& oldHash, const std::string& newHash, const std::string& pathPrefix)
{
	// Diff against the last processed commit tree, or the empty tree if there is none
	git_diff * Diff;
	{
		git_diff_options Options;
		git_diff_options_init(&Options, GIT_DIFF_OPTIONS_VERSION);
		git_tree * SourceTree = nullptr;

		git_tree * DestTree;
		const std::string DestTreeish = concat(newHash, ':', ModRoot);
		convertTreeishToTree(&DestTree, Repo, DestTreeish.c_str());
		auto && DestGuard = makeScopeGuard([&] { git_tree_free(DestTree); });

		auto && SourceGuard = makeScopeGuard([&] { git_tree_free(SourceTree); });

		if (!oldHash.empty()) {
			std::string SourceTreeish = concat(oldHash, ':', ModRoot);;
			convertTreeishToTree(&SourceTree, Repo, SourceTreeish.c_str());
		}
		checkRet(git_diff_tree_to_tree(&Diff, Repo, SourceTree, DestTree, &Options), "git_diff_tree_to_tree");
	}

	SubmoduleContext submoduleContext{Store, Archive, ModRoot, pathPrefix, SubmoduleHashes()};

	processDiff(Diff, Store, Archive, Repo, submoduleContext.submoduleHashes, pathPrefix);

	auto submodule_cb = [](git_submodule *sm, const char *name, void *payload)
	{
		const auto sc = reinterpret_cast<SubmoduleContext*>(payload);
		if (sc->submoduleHashes.find(name) == sc->submoduleHashes.end())
			return 0;

		std::cout << "Entering submodule:\t" << name << "\n";
		git_repository * SubmoduleRepo;
		git_submodule_open(&SubmoduleRepo, sm);
		const auto& hashes = sc->submoduleHashes[std::string(name)];
		std::cout << hashes.first << " " << hashes.second << "\n";

		processRepo(sc->Store, sc->Archive, SubmoduleRepo, sc->ModRoot, hashes.first, hashes.second, concatPrefix(sc->pathPrefix, name));

		return 0;
	};
	git_submodule_foreach(Repo, submodule_cb, &submoduleContext);
}


void buildGit(
	std::string const & GitPath,
	std::string const & ModRoot,
	std::string const & Modinfo,
	std::string const & StorePath,
	std::string const & GitHash,
	std::string const & Prefix)
{
	// Initialize libgit2
	checkRet(git_libgit2_init() == 1 ? 0 : -1, "git_libgit2_init()");
	auto && ThreadsGuard = makeScopeGuard([&] { git_libgit2_shutdown(); });

	// Load the git repo
	git_repository * Repo;
	checkRet(git_repository_open_ext(&Repo, GitPath.c_str(), 0, nullptr), "git_repository_open_ext");
	auto && RepoGuard = makeScopeGuard([&] { git_repository_free(Repo); });

	// Lookup destination oid
	git_oid DestOid;
	checkRet(git_oid_fromstr(&DestOid, GitHash.c_str()), "git_oid_fromstr");

	// Make a short hash
	std::array<char, 7> ShortHash;
	std::copy(GitHash.data(), GitHash.data() + 7, ShortHash.data());

	// Find the commit count
	git_revwalk * Walker;
	checkRet(git_revwalk_new(&Walker, Repo), "git_revwalk_new");
	auto && WalkerGuard = makeScopeGuard([&] { git_revwalk_free(Walker); });
	checkRet(git_revwalk_push(Walker, &DestOid), "git_revwalk_push");

	std::size_t CommitCount = 0;
	while (true)
	{
		git_oid WalkerOid;
		int Ret = git_revwalk_next(&WalkerOid, Walker);
		if (Ret == GIT_ITEROVER) break;
		checkRet(Ret, "git_revwalk_next");
		++CommitCount;
	}

	// Extract the commit type from the commit message
	git_commit * Commit;
	checkRet(git_commit_lookup(&Commit, Repo, &DestOid), "git_commit_lookup");
	auto && CommitGuard = makeScopeGuard([&] { git_commit_free(Commit); });
	//std::size_t AncestorCount = git_commit_parentcount(Commit);
	std::string TestVersion = concat(std::to_string(CommitCount), '-', ShortHash);
	auto CommitInfo = extractVersion(git_commit_message_raw(Commit), TestVersion);

	// Initialize the store
	StoreT Store{StorePath};
	Store.init();

	// Load the destination commit tree
	git_tree * DestTree;
	std::string const DestTreeish = concat(GitHash, ':', ModRoot);
	convertTreeishToTree(&DestTree, Repo, DestTreeish.c_str());
	auto && DestGuard = makeScopeGuard([&] { git_tree_free(DestTree); });
	// Prepare to perform diff
	PoolArchiveT Archive{Store};

	auto Option = LastGitT::load(Store, Prefix);
	std::string oldHash;

	if (!Option) {
			std::cout << "Unable to perform incremental an update\n";
	} else {
		auto & Last = *Option;
		Archive.load(Last.Digest);
		oldHash.resize(40);
		Hex::encode(&oldHash[0], Last.Hex.data(), 20);

		std::cout <<
			"Performing incremental update: " <<
			oldHash <<
			"..." <<
			GitHash <<
			"\n";
	}
	processRepo(Store, Archive, Repo, ModRoot, oldHash, GitHash, "");


	// Update modinfo.lua with $VERSION replacement
	git_tree_entry * TreeEntry;
	checkRet(git_tree_entry_bypath(&TreeEntry, DestTree, Modinfo.c_str()), "git_tree_entry_bypath");
	git_blob * Blob;
	checkRet(git_blob_lookup(&Blob, Repo, git_tree_entry_id(TreeEntry)), "git_blob_lookup");
	auto && BlobGuard = makeScopeGuard([&] { git_blob_free(Blob); });
	auto Size = git_blob_rawsize(Blob);
	auto Pointer = static_cast<char const *>(git_blob_rawcontent(Blob));
	PoolFileT File{Store};
	replaceVersion(Pointer, Size, CommitInfo.Version, [&](char const * Data, std::size_t Length)
	{
		File.write(Data, Length);
	});
	auto FileEntry = File.close();
	Archive.add(Modinfo, FileEntry);
	auto ArchiveEntry = Archive.save();

	// Add tags and save versions.gz
	VersionsT Versions{Store};
	Versions.load();
	std::string const Tag = concat(Prefix, ':', CommitInfo.Branch);
	std::string const Tag2 = concat(Prefix, ":git:", GitHash);
	Versions.add(Tag, ArchiveEntry);
	Versions.add(Tag2, ArchiveEntry);
	Versions.save();

	// Save info for next incremental run
	LastGitT Last;
	Hex::decode(GitHash.c_str(), Last.Hex.data(), 20);
	Last.Digest = ArchiveEntry.Digest;
	LastGitT::save(Last, Store, Prefix);

	// Create zip if needed
	if (CommitInfo.MakeZip)
	{
		auto Path = Store.getBuildPath(Prefix, CommitInfo.Version);
		std::cout << "Generating zip file: " << Path << "\n";
		Archive.makeZip(Path);
		// Call upload.py?
	}
}

}

std::string fixRoot(std::string const & root)
{
	if ((root == ".") || (root == "/")) return "";
	else return root;
}


int main(int argc, char const * const * argv)
{
	umask(0002);

	if (argc != 7)
	{
		std::cerr << "Usage: " << argv[0] <<
			" <Git Path> <Mod Root> <Modinfo> <Store Path> <Git Hash> <Prefix>\n";
		return 1;
	}

	try
	{
		buildGit(
			argv[1],
			fixRoot(argv[2]),
			argv[3],
			argv[4],
			argv[5],
			argv[6]);
	}
	catch (std::exception const & Exception)
	{
		std::cerr << Exception.what() << "\n";
		return 1;
	}

	return 0;
}
