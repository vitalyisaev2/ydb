#include "yql_credentials.h"

namespace NYql {

void TCredentials::AddCredential(const TString& alias, const TCredential& cred) {
    Cout << "TCredentials::AddCredential: " << alias << ": " << cred.Content << Endl;
    CredentialTable_.emplace(alias, cred);
}

const TCredential* TCredentials::FindCredential(const TStringBuf& name) const {
    Cout << "TCredentials::FindCredential: " << name << " in map of " << CredentialTable_.size() << " elements" << Endl;
    return CredentialTable_.FindPtr(name);
}

TString TCredentials::FindCredentialContent(const TStringBuf& name1, const TStringBuf& name2, const TString& defaultContent) const {
    Cout << "TCredentials::FindCredentialContent: " << name1 << " " << name2 << Endl;
    if (auto cred = FindCredential(name1)) {
        return cred->Content;
    }

    if (auto cred = FindCredential(name2)) {
        return cred->Content;
    }

    return defaultContent;
}

void TCredentials::ForEach(const std::function<void(const TString, const TCredential&)>& callback) const {
    for (const auto& x : CredentialTable_) {
        callback(x.first, x.second);
    }
}

} // namespace NYql
