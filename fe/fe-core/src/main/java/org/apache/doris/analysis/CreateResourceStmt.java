// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.analysis;

import org.apache.doris.catalog.Env;
import org.apache.doris.catalog.Resource.ResourceType;
import org.apache.doris.common.AnalysisException;
import org.apache.doris.common.ErrorCode;
import org.apache.doris.common.ErrorReport;
import org.apache.doris.common.FeNameFormat;
import org.apache.doris.common.UserException;
import org.apache.doris.common.util.PrintableMap;
import org.apache.doris.datasource.property.constants.AzureProperties;
import org.apache.doris.mysql.privilege.PrivPredicate;
import org.apache.doris.qe.ConnectContext;

import com.google.common.base.Strings;
import com.google.common.collect.ImmutableMap;

import java.util.Map;

// CREATE [EXTERNAL] RESOURCE resource_name
// PROPERTIES (key1 = value1, ...)
public class CreateResourceStmt extends DdlStmt implements NotFallbackInParser {
    private static final String TYPE = "type";

    private final boolean isExternal;
    private final boolean ifNotExists;
    private final String resourceName;
    private final ImmutableMap<String, String> properties;
    private ResourceType resourceType;

    public CreateResourceStmt(boolean isExternal, boolean ifNotExists, String resourceName,
            Map<String, String> properties) {
        this.isExternal = isExternal;
        this.ifNotExists = ifNotExists;
        this.resourceName = resourceName;
        this.properties = ImmutableMap.copyOf(properties);
        this.resourceType = ResourceType.UNKNOWN;
    }

    public boolean isIfNotExists() {
        return ifNotExists;
    }

    public String getResourceName() {
        return resourceName;
    }

    public ImmutableMap<String, String> getProperties() {
        return properties;
    }

    public ResourceType getResourceType() {
        return resourceType;
    }

    public void analyzeResourceType() throws UserException {
        String type = null;
        for (Map.Entry<String, String> property : properties.entrySet()) {
            if (property.getKey().equalsIgnoreCase(TYPE)) {
                type = property.getValue();
            }
        }
        if (Strings.isNullOrEmpty(type)) {
            throw new AnalysisException("Resource type can't be null");
        }

        if (AzureProperties.checkAzureProviderPropertyExist(properties)) {
            resourceType = ResourceType.AZURE;
            return;
        }

        resourceType = ResourceType.fromString(type);
        if (resourceType == ResourceType.UNKNOWN || resourceType == ResourceType.SPARK) {
            throw new AnalysisException("Unsupported resource type: " + type);
        }
        if (resourceType == ResourceType.ODBC_CATALOG) {
            throw new AnalysisException("ODBC table is deprecated, use JDBC instead.");
        }
    }

    @Override
    public void analyze(Analyzer analyzer) throws UserException {
        super.analyze(analyzer);

        // check auth
        if (!Env.getCurrentEnv().getAccessManager().checkGlobalPriv(ConnectContext.get(), PrivPredicate.ADMIN)) {
            ErrorReport.reportAnalysisException(ErrorCode.ERR_SPECIFIC_ACCESS_DENIED_ERROR, "ADMIN");
        }

        // check name
        FeNameFormat.checkResourceName(resourceName, ResourceTypeEnum.GENERAL);

        // check type in properties
        if (properties == null || properties.isEmpty()) {
            throw new AnalysisException("Resource properties can't be null");
        }

        analyzeResourceType();
    }

    @Override
    public String toSql() {
        StringBuilder sb = new StringBuilder();
        sb.append("CREATE ");
        if (isExternal) {
            sb.append("EXTERNAL ");
        }
        sb.append("RESOURCE '").append(resourceName).append("' ");
        sb.append("PROPERTIES(").append(new PrintableMap<>(properties, " = ", true, false, true)).append(")");
        return sb.toString();
    }

    @Override
    public boolean needAuditEncryption() {
        return true;
    }

    @Override
    public StmtType stmtType() {
        return StmtType.CREATE;
    }
}
