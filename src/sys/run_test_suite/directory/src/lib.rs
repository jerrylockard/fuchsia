// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod macros;
pub mod testing;
mod v1;

use {
    serde::{Deserialize, Serialize},
    std::{
        borrow::Cow,
        collections::HashMap,
        fs::{DirBuilder, File},
        io::Error,
        path::{Path, PathBuf},
    },
    test_list::TestTag,
};

/// Filename of the top level summary json.
pub const RUN_SUMMARY_NAME: &str = "run_summary.json";
pub const RUN_NAME: &str = "run";

enumerable_enum! {
    /// Schema version.
    #[derive(PartialEq, Eq, Debug, Clone, Copy, Hash)]
    SchemaVersion {
        V1,
    }
}

enumerable_enum! {
    /// A serializable version of a test outcome.
    #[derive(Deserialize, Serialize, PartialEq, Eq, Debug, Clone, Copy)]
    #[serde(rename_all = "SCREAMING_SNAKE_CASE")]
    Outcome {
        NotStarted,
        Passed,
        Failed,
        Inconclusive,
        Timedout,
        Error,
        Skipped,
    }
}

enumerable_enum! {
    /// Types of artifacts known to the test framework.
    #[derive(Deserialize, Serialize, PartialEq, Eq, Debug, Clone, Copy, Hash)]
    #[serde(rename_all = "SCREAMING_SNAKE_CASE")]
    ArtifactType {
        Syslog,
        /// Unexpected high severity logs that caused a test to fail.
        RestrictedLog,
        Stdout,
        Stderr,
        /// A directory containing custom artifacts produced by a component in the test.
        Custom,
        /// A human readable report generated by the test framework.
        Report,
        /// Debug data. For example, profraw or symbolizer output.
        Debug,
    }
}

/// A subdirectory of an output directory that contains artifacts for a test run,
/// test suite, or test case.
#[derive(PartialEq, Eq, Debug, Clone)]
pub struct ArtifactSubDirectory {
    version: SchemaVersion,
    root: PathBuf,
    artifacts: HashMap<PathBuf, ArtifactMetadata>,
}

/// Contains result information common to all results. It's useful to store
#[derive(PartialEq, Eq, Debug, Clone)]
pub struct CommonResult {
    pub name: String,
    pub artifact_dir: ArtifactSubDirectory,
    pub outcome: MaybeUnknown<Outcome>,
    /// Approximate start time, as milliseconds since the epoch.
    pub start_time: Option<u64>,
    pub duration_milliseconds: Option<u64>,
}

/// A serializable test run result.
/// This contains overall results and artifacts scoped to a test run, and
/// a list of filenames for finding serialized suite results.
#[derive(PartialEq, Eq, Debug, Clone)]
pub struct TestRunResult<'a> {
    pub common: Cow<'a, CommonResult>,
    pub suites: Vec<SuiteResult<'a>>,
}

/// A serializable suite run result.
/// Contains overall results and artifacts scoped to a suite run, and
/// results and artifacts scoped to any test run within it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SuiteResult<'a> {
    pub common: Cow<'a, CommonResult>,
    pub cases: Vec<TestCaseResult<'a>>,
    pub tags: Cow<'a, Vec<TestTag>>,
}

/// A serializable test case result.
#[derive(PartialEq, Eq, Debug, Clone)]
pub struct TestCaseResult<'a> {
    pub common: Cow<'a, CommonResult>,
}

impl TestRunResult<'static> {
    pub fn from_dir(root: &Path) -> Result<Self, Error> {
        v1::parse_from_directory(root)
    }
}

/// Metadata associated with an artifact.
#[derive(Deserialize, Serialize, PartialEq, Eq, Debug, Clone, Hash)]
pub struct ArtifactMetadata {
    /// The type of the artifact.
    pub artifact_type: MaybeUnknown<ArtifactType>,
    /// Moniker of the component which produced the artifact, if applicable.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub component_moniker: Option<String>,
}

#[derive(Deserialize, Serialize, PartialEq, Eq, Debug, Hash, Clone)]
#[serde(untagged)]
pub enum MaybeUnknown<T> {
    Known(T),
    Unknown(String),
}

impl<T> From<T> for MaybeUnknown<T> {
    fn from(other: T) -> Self {
        Self::Known(other)
    }
}

impl From<ArtifactType> for ArtifactMetadata {
    fn from(other: ArtifactType) -> Self {
        Self { artifact_type: MaybeUnknown::Known(other), component_moniker: None }
    }
}

/// A helper for accumulating results in an output directory.
///
/// |OutputDirectoryBuilder| handles details specific to the format of the test output
/// format, such as the locations of summaries and artifacts, while allowing the caller to
/// accumulate results separately. A typical usecase might look like this:
/// ```rust
/// let output_directory = OutputDirectoryBuilder::new("/path", SchemaVersion::V1)?;
/// let mut run_result = TestRunResult {
///     common: Cow::Owned(CommonResult{
///         name: "run".to_string(),
///         artifact_dir: output_directory.new_artifact_dir("run-artifacts")?,
///         outcome: Outcome::Inconclusive.into(),
///         start_time: None,
///         duration_milliseconds: None,
///     }),
///     suites: vec![],
/// };
///
/// // accumulate results in run_result over time... then save the summary.
/// output_directory.save_summary(&run_result)?;
/// ```
pub struct OutputDirectoryBuilder {
    version: SchemaVersion,
    root: PathBuf,
    /// Creation instant exists purely to make pseudo random directory names to discourage parsing
    /// methods that rely on unstable directory names and could be broken when internal
    /// implementation changes.
    creation_instant: std::time::Instant,
}

impl OutputDirectoryBuilder {
    /// Register a directory for use as an output directory using version |version|.
    pub fn new(dir: impl Into<PathBuf>, version: SchemaVersion) -> Result<Self, Error> {
        let root = dir.into();
        ensure_directory_exists(&root)?;
        Ok(Self { version, root, creation_instant: std::time::Instant::now() })
    }

    /// Create a new artifact subdirectory.
    ///
    /// The new |ArtifactSubDirectory| should be referenced from either the test run, suite, or
    /// case when a summary is saved in this OutputDirectoryBuilder with |save_summary|.
    pub fn new_artifact_dir(&self) -> Result<ArtifactSubDirectory, Error> {
        match self.version {
            SchemaVersion::V1 => {
                let subdir_root =
                    self.root.join(format!("{:?}", self.creation_instant.elapsed().as_nanos()));
                Ok(ArtifactSubDirectory {
                    version: self.version,
                    root: subdir_root,
                    artifacts: HashMap::new(),
                })
            }
        }
    }

    /// Save a summary of the test results in the directory.
    pub fn save_summary<'a, 'b>(&'a self, result: &'a TestRunResult<'b>) -> Result<(), Error> {
        match self.version {
            SchemaVersion::V1 => v1::save_summary(self.root.as_path(), result),
        }
    }

    /// Get the path to the root directory.
    pub fn path(&self) -> &Path {
        self.root.as_path()
    }
}

impl ArtifactSubDirectory {
    /// Create a new file based artifact.
    pub fn new_artifact(
        &mut self,
        metadata: impl Into<ArtifactMetadata>,
        name: impl AsRef<Path>,
    ) -> Result<File, Error> {
        ensure_directory_exists(self.root.as_path())?;
        match self.version {
            SchemaVersion::V1 => {
                // todo validate path
                self.artifacts.insert(name.as_ref().to_path_buf(), metadata.into());
                File::create(self.root.join(name))
            }
        }
    }

    /// Create a new directory based artifact.
    pub fn new_directory_artifact(
        &mut self,
        metadata: impl Into<ArtifactMetadata>,
        name: impl AsRef<Path>,
    ) -> Result<PathBuf, Error> {
        match self.version {
            SchemaVersion::V1 => {
                // todo validate path
                let subdir = self.root.join(name.as_ref());
                ensure_directory_exists(subdir.as_path())?;
                self.artifacts.insert(name.as_ref().to_path_buf(), metadata.into());
                Ok(subdir)
            }
        }
    }

    /// Get the absolute path of the artifact at |name|, if present.
    pub fn path_to_artifact(&self, name: impl AsRef<Path>) -> Option<PathBuf> {
        match self.version {
            SchemaVersion::V1 => match self.artifacts.contains_key(name.as_ref()) {
                true => Some(self.root.join(name.as_ref())),
                false => None,
            },
        }
    }

    /// Return a list of paths of artifacts in the directory, relative to the root of the artifact
    /// directory.
    pub fn contents(&self) -> Vec<PathBuf> {
        self.artifacts.keys().cloned().collect()
    }
}

fn ensure_directory_exists(dir: &Path) -> Result<(), Error> {
    match dir.exists() {
        true => Ok(()),
        false => DirBuilder::new().recursive(true).create(&dir),
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use std::{io::Write, path::Path};
    use tempfile::tempdir;

    fn validate_against_schema(version: SchemaVersion, root: &Path) {
        match version {
            SchemaVersion::V1 => v1::validate_against_schema(root),
        }
    }

    /// Run a round trip test against all known schema versions.
    fn round_trip_test_all_versions<F>(produce_run_fn: F)
    where
        F: Fn(&OutputDirectoryBuilder) -> TestRunResult<'static>,
    {
        for version in SchemaVersion::all_variants() {
            let dir = tempdir().expect("Create dir");
            let output_dir = OutputDirectoryBuilder::new(dir.path(), version).expect("create dir");

            let run_result = produce_run_fn(&output_dir);
            output_dir.save_summary(&run_result).expect("save summary");

            validate_against_schema(version, dir.path());

            let parsed = TestRunResult::from_dir(dir.path()).expect("parse output directory");
            assert_eq!(run_result, parsed, "version: {:?}", version);
        }
    }

    #[test]
    fn minimal() {
        round_trip_test_all_versions(|dir_builder| TestRunResult {
            common: Cow::Owned(CommonResult {
                name: RUN_NAME.to_string(),
                artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                outcome: Outcome::Passed.into(),
                start_time: None,
                duration_milliseconds: None,
            }),
            suites: vec![],
        });
        let _ = Outcome::all_variants();
    }

    #[test]
    fn artifact_types() {
        for artifact_type in ArtifactType::all_variants() {
            round_trip_test_all_versions(|dir_builder| {
                let mut run_artifact_dir = dir_builder.new_artifact_dir().expect("new dir");
                let mut run_artifact =
                    run_artifact_dir.new_artifact(artifact_type, "a.txt").expect("create artifact");
                write!(run_artifact, "run contents").unwrap();

                let mut suite_artifact_dir = dir_builder.new_artifact_dir().expect("new dir");
                let mut suite_artifact = suite_artifact_dir
                    .new_artifact(artifact_type, "a.txt")
                    .expect("create artifact");
                write!(suite_artifact, "suite contents").unwrap();

                let mut case_artifact_dir = dir_builder.new_artifact_dir().expect("new dir");
                let mut case_artifact = case_artifact_dir
                    .new_artifact(artifact_type, "a.txt")
                    .expect("create artifact");
                write!(case_artifact, "case contents").unwrap();

                TestRunResult {
                    common: Cow::Owned(CommonResult {
                        name: RUN_NAME.to_string(),
                        artifact_dir: run_artifact_dir,
                        outcome: Outcome::Passed.into(),
                        start_time: None,
                        duration_milliseconds: None,
                    }),
                    suites: vec![SuiteResult {
                        common: Cow::Owned(CommonResult {
                            name: "suite".to_string(),
                            artifact_dir: suite_artifact_dir,
                            outcome: Outcome::Passed.into(),
                            start_time: None,
                            duration_milliseconds: None,
                        }),
                        tags: Cow::Owned(vec![]),
                        cases: vec![TestCaseResult {
                            common: Cow::Owned(CommonResult {
                                name: "case".to_string(),
                                artifact_dir: case_artifact_dir,
                                outcome: Outcome::Passed.into(),
                                start_time: None,
                                duration_milliseconds: None,
                            }),
                        }],
                    }],
                }
            });
        }
    }

    #[test]
    fn artifact_types_moniker_specified() {
        for artifact_type in ArtifactType::all_variants() {
            round_trip_test_all_versions(|dir_builder| {
                let mut run_artifact_dir = dir_builder.new_artifact_dir().expect("new dir");
                let mut run_artifact = run_artifact_dir
                    .new_artifact(
                        ArtifactMetadata {
                            artifact_type: artifact_type.into(),
                            component_moniker: Some("moniker".to_string()),
                        },
                        "a.txt",
                    )
                    .expect("create artifact");
                write!(run_artifact, "run contents").unwrap();

                let mut suite_artifact_dir = dir_builder.new_artifact_dir().expect("new dir");
                let mut suite_artifact = suite_artifact_dir
                    .new_artifact(
                        ArtifactMetadata {
                            artifact_type: artifact_type.into(),
                            component_moniker: Some("moniker".to_string()),
                        },
                        "a.txt",
                    )
                    .expect("create artifact");
                write!(suite_artifact, "suite contents").unwrap();

                let mut case_artifact_dir = dir_builder.new_artifact_dir().expect("new dir");
                let mut case_artifact = case_artifact_dir
                    .new_artifact(
                        ArtifactMetadata {
                            artifact_type: artifact_type.into(),
                            component_moniker: Some("moniker".to_string()),
                        },
                        "a.txt",
                    )
                    .expect("create artifact");
                write!(case_artifact, "case contents").unwrap();

                TestRunResult {
                    common: Cow::Owned(CommonResult {
                        name: RUN_NAME.to_string(),
                        artifact_dir: run_artifact_dir,
                        outcome: Outcome::Passed.into(),
                        start_time: None,
                        duration_milliseconds: None,
                    }),
                    suites: vec![SuiteResult {
                        common: Cow::Owned(CommonResult {
                            name: "suite".to_string(),
                            artifact_dir: suite_artifact_dir,
                            outcome: Outcome::Passed.into(),
                            start_time: None,
                            duration_milliseconds: None,
                        }),
                        tags: Cow::Owned(vec![]),
                        cases: vec![TestCaseResult {
                            common: Cow::Owned(CommonResult {
                                name: "case".to_string(),
                                artifact_dir: case_artifact_dir,
                                outcome: Outcome::Passed.into(),
                                start_time: None,
                                duration_milliseconds: None,
                            }),
                        }],
                    }],
                }
            });
        }
    }

    #[test]
    fn outcome_types() {
        for outcome_type in Outcome::all_variants() {
            round_trip_test_all_versions(|dir_builder| TestRunResult {
                common: Cow::Owned(CommonResult {
                    name: RUN_NAME.to_string(),
                    artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                    outcome: outcome_type.into(),
                    start_time: None,
                    duration_milliseconds: None,
                }),
                suites: vec![SuiteResult {
                    common: Cow::Owned(CommonResult {
                        name: "suite".to_string(),
                        artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                        outcome: outcome_type.into(),
                        start_time: None,
                        duration_milliseconds: None,
                    }),
                    tags: Cow::Owned(vec![]),
                    cases: vec![TestCaseResult {
                        common: Cow::Owned(CommonResult {
                            name: "case".to_string(),
                            artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                            outcome: outcome_type.into(),
                            start_time: None,
                            duration_milliseconds: None,
                        }),
                    }],
                }],
            });
        }
    }

    #[test]
    fn timing_specified() {
        round_trip_test_all_versions(|dir_builder| TestRunResult {
            common: Cow::Owned(CommonResult {
                name: RUN_NAME.to_string(),
                artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                outcome: Outcome::Passed.into(),
                start_time: Some(1),
                duration_milliseconds: Some(2),
            }),
            suites: vec![SuiteResult {
                common: Cow::Owned(CommonResult {
                    name: "suite".to_string(),
                    artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                    outcome: Outcome::Passed.into(),
                    start_time: Some(3),
                    duration_milliseconds: Some(4),
                }),
                tags: Cow::Owned(vec![]),
                cases: vec![TestCaseResult {
                    common: Cow::Owned(CommonResult {
                        name: "case".to_string(),
                        artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                        outcome: Outcome::Passed.into(),
                        start_time: Some(5),
                        duration_milliseconds: Some(6),
                    }),
                }],
            }],
        });
    }

    #[test]
    fn tags_specified() {
        round_trip_test_all_versions(|dir_builder| TestRunResult {
            common: Cow::Owned(CommonResult {
                name: RUN_NAME.to_string(),
                artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                outcome: Outcome::Passed.into(),
                start_time: Some(1),
                duration_milliseconds: Some(2),
            }),
            suites: vec![SuiteResult {
                common: Cow::Owned(CommonResult {
                    name: "suite".to_string(),
                    artifact_dir: dir_builder.new_artifact_dir().expect("new dir"),
                    outcome: Outcome::Passed.into(),
                    start_time: Some(3),
                    duration_milliseconds: Some(4),
                }),
                tags: Cow::Owned(vec![
                    TestTag { key: "hermetic".to_string(), value: "false".to_string() },
                    TestTag { key: "realm".to_string(), value: "system".to_string() },
                ]),
                cases: vec![],
            }],
        });
    }
}
